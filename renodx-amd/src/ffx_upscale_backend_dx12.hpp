/*
 * Experimental FidelityFX DX12 upscaler backend for the RenoDX Streamline bridge.
 *
 * The backend consumes game-provided Streamline DLSS inputs; it does NOT run
 * NVIDIA's proprietary DLSS implementation on AMD hardware.
 */

#pragma once

#include "amd_bridge.hpp"
#include "ffx_runtime_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>

#if RENODX_AMD_HAS_FFX_API && __has_include(<ffx_upscale.h>) && __has_include(<dx12/ffx_api_dx12.h>)
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>
#define RENODX_AMD_HAS_FFX_UPSCALE_DX12 1
#elif RENODX_AMD_HAS_FFX_API && __has_include("ffx_upscale.h") && __has_include("dx12/ffx_api_dx12.h")
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"
#define RENODX_AMD_HAS_FFX_UPSCALE_DX12 1
#else
#define RENODX_AMD_HAS_FFX_UPSCALE_DX12 0
#endif

namespace renodx::utils::dlss::amd_bridge::ffx_dx12 {

constexpr uint32_t kAmdVendorId = 0x1002u;

enum class InputColorSpace {
  kLinear,
  kSRGB,
  kPQ,
};

enum class FovConvention {
  kVertical,
  kHorizontal,
};

struct BackendConfig {
  bool high_dynamic_range = false;
  bool dynamic_resolution = true;
  bool depth_infinite = false;
  bool auto_exposure_when_missing = true;
  bool enable_sharpening = false;
  float sharpness = 0.0f;
  float frame_time_delta_ms = 16.666667f;
  float pre_exposure = 1.0f;
  float view_space_to_meters = 1.0f;
  InputColorSpace input_color_space = InputColorSpace::kLinear;
  FovConvention fov_convention = FovConvention::kVertical;
};

#if RENODX_AMD_HAS_FFX_UPSCALE_DX12

namespace internal {

struct Dimensions {
  uint32_t width = 0u;
  uint32_t height = 0u;

  [[nodiscard]] bool IsValid() const { return width != 0u && height != 0u; }
  bool operator==(const Dimensions&) const = default;
};

struct ContextRecord {
  ffxContext context = nullptr;
  Dimensions max_render_size{};
  Dimensions max_upscale_size{};
  uint32_t create_flags = 0u;
};

struct RestoreTransition {
  ID3D12Resource* resource = nullptr;
  D3D12_RESOURCE_STATES original = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES ffx_state = D3D12_RESOURCE_STATE_COMMON;
};

inline std::mutex backend_mutex;
inline ID3D12Device* device = nullptr;
inline BackendConfig config{};
inline bool initialized = false;
inline std::unordered_map<uint32_t, ContextRecord> contexts;

inline bool IsAmdDevice(ID3D12Device* candidate) {
  if (candidate == nullptr) return false;

  IDXGIFactory4* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || factory == nullptr) return false;

  IDXGIAdapter1* adapter = nullptr;
  const auto result = factory->EnumAdapterByLuid(candidate->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
  factory->Release();
  if (FAILED(result) || adapter == nullptr) return false;

  DXGI_ADAPTER_DESC1 desc{};
  const auto desc_result = adapter->GetDesc1(&desc);
  adapter->Release();
  return SUCCEEDED(desc_result) && desc.VendorId == kAmdVendorId;
}

inline ID3D12Resource* NativeResource(const CapturedResource& captured) {
  if (!captured.IsValid()) return nullptr;
  return reinterpret_cast<ID3D12Resource*>(captured.resource->native);
}

inline bool IsKnownState(const CapturedResource& captured) {
  return captured.IsValid() && captured.resource->state != std::numeric_limits<uint32_t>::max();
}

inline bool IsUsableOptionalResource(const CapturedResource& captured) {
  return IsKnownState(captured) && NativeResource(captured) != nullptr;
}

inline bool HasUnsupportedOffset(const CapturedResource& captured) {
  if (!captured.extent.has_value() || !static_cast<bool>(*captured.extent)) return false;
  return captured.extent->left != 0u || captured.extent->top != 0u;
}

inline Dimensions ResourceDimensions(const CapturedResource& captured) {
  if (captured.extent.has_value() && static_cast<bool>(*captured.extent)) {
    return {captured.extent->width, captured.extent->height};
  }

  auto* resource = NativeResource(captured);
  if (resource == nullptr) return {};
  const auto desc = resource->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return {};
  return {
      static_cast<uint32_t>(std::min<uint64_t>(desc.Width, std::numeric_limits<uint32_t>::max())),
      desc.Height};
}

inline bool IsValidSlFloat(float value) {
  return std::isfinite(value) && value != sl::INVALID_FLOAT;
}

inline const sl::DLSSOptions* DlssOptions(const FrameState& state) {
  return state.dlss_options.has_value() ? &*state.dlss_options : nullptr;
}

inline bool IsHdr(const FrameState& state) {
  const auto* options = DlssOptions(state);
  return config.high_dynamic_range
      || (options != nullptr && options->colorBuffersHDR == sl::Boolean::eTrue);
}

inline bool UseAutoExposure(const FrameState& state) {
  const auto* options = DlssOptions(state);
  if (options != nullptr && options->useAutoExposure == sl::Boolean::eTrue) return true;
  return !IsUsableOptionalResource(state.resources.exposure)
      && config.auto_exposure_when_missing;
}

inline float EffectivePreExposure(const FrameState& state) {
  const auto* options = DlssOptions(state);
  if (options != nullptr && std::isfinite(options->preExposure) && options->preExposure > 0.0f) {
    return options->preExposure;
  }
  return std::max(config.pre_exposure, 0.0001f);
}

inline float VerticalFov(const sl::Constants& constants) {
  if (config.fov_convention == FovConvention::kVertical) return constants.cameraFOV;
  if (!IsValidSlFloat(constants.cameraAspectRatio) || constants.cameraAspectRatio <= 0.0f) {
    return constants.cameraFOV;
  }
  return 2.0f * std::atan(
      std::tan(constants.cameraFOV * 0.5f) / constants.cameraAspectRatio);
}

inline uint32_t BuildCreateFlags(const FrameState& state) {
  const auto& constants = *state.constants;
  uint32_t flags = 0u;

  if (IsHdr(state)) flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
  if (config.dynamic_resolution) flags |= FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
  if (config.depth_infinite) flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
  if (constants.depthInverted == sl::Boolean::eTrue) flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
  if (constants.motionVectorsJittered == sl::Boolean::eTrue) {
    flags |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
  }
  if (config.input_color_space != InputColorSpace::kLinear) {
    flags |= FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
  }

  const auto render_size = ResourceDimensions(state.resources.scaling_input);
  const auto output_size = ResourceDimensions(state.resources.scaling_output);
  const auto mv_size = ResourceDimensions(state.resources.motion_vectors);
  if (mv_size.IsValid() && output_size.IsValid() && render_size.IsValid()
      && mv_size == output_size && !(mv_size == render_size)) {
    flags |= FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
  }

  if (UseAutoExposure(state)) flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
  return flags;
}

inline sl::Result MapFfxResult(ffxReturnCode_t result) {
  switch (result) {
    case FFX_API_RETURN_OK:
      return sl::Result::eOk;
    case FFX_API_RETURN_ERROR_PARAMETER:
      return sl::Result::eErrorInvalidParameter;
    case FFX_API_RETURN_NO_PROVIDER:
      return sl::Result::eErrorFeatureNotSupported;
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR:
      return sl::Result::eErrorD3DAPI;
    default:
      return sl::Result::eErrorComputeFailed;
  }
}

inline void DestroyContextLocked(ContextRecord& record) {
  if (record.context == nullptr) return;
  const auto& runtime = ffx::GetRuntime();
  if (runtime.destroy_context != nullptr) runtime.destroy_context(&record.context, nullptr);
  record = {};
}

inline void DestroyAllContextsLocked() {
  for (auto& [_, record] : contexts) DestroyContextLocked(record);
  contexts.clear();
}

inline bool ProviderAvailableLocked() {
  const auto& runtime = ffx::GetRuntime();
  if (!runtime.IsReady() || device == nullptr) return false;

  uint64_t version_count = 0u;
  ffxQueryDescGetVersions query{};
  query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  query.device = device;
  query.outputCount = &version_count;

  return runtime.query(nullptr, &query.header) == FFX_API_RETURN_OK
      && version_count != 0u;
}

inline bool CreateContextLocked(
    uint32_t viewport,
    Dimensions render_size,
    Dimensions output_size,
    uint32_t create_flags) {
  const auto& runtime = ffx::GetRuntime();
  if (!runtime.IsReady() || device == nullptr) return false;

  auto& record = contexts[viewport];
  if (record.context != nullptr
      && record.create_flags == create_flags
      && render_size.width <= record.max_render_size.width
      && render_size.height <= record.max_render_size.height
      && output_size.width <= record.max_upscale_size.width
      && output_size.height <= record.max_upscale_size.height) {
    return true;
  }

  DestroyContextLocked(record);

  ffxCreateContextDescUpscale upscale{};
  upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  upscale.flags = create_flags;
  upscale.maxRenderSize = {render_size.width, render_size.height};
  upscale.maxUpscaleSize = {output_size.width, output_size.height};
  upscale.fpMessage = nullptr;

  // SDK 2.1+ requires the public upscaler API version descriptor.
  ffxCreateContextDescUpscaleVersion version{};
  version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
  version.version = FFX_UPSCALER_VERSION;

  ffxCreateBackendDX12Desc backend{};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.device = device;

  upscale.header.pNext = &version.header;
  version.header.pNext = &backend.header;

  ffxContext new_context = nullptr;
  const auto result = runtime.create_context(&new_context, &upscale.header, nullptr);
  if (result != FFX_API_RETURN_OK || new_context == nullptr) return false;

  record.context = new_context;
  record.max_render_size = render_size;
  record.max_upscale_size = output_size;
  record.create_flags = create_flags;
  return true;
}

inline bool RequiredResourcesReady(const FrameState& state) {
  const auto& resources = state.resources;
  if (!IsKnownState(resources.scaling_input)
      || !IsKnownState(resources.scaling_output)
      || !IsKnownState(resources.depth)
      || !IsKnownState(resources.motion_vectors)) {
    return false;
  }

  if (HasUnsupportedOffset(resources.scaling_input)
      || HasUnsupportedOffset(resources.scaling_output)
      || HasUnsupportedOffset(resources.depth)
      || HasUnsupportedOffset(resources.motion_vectors)) {
    return false;
  }

  const auto render_size = ResourceDimensions(resources.scaling_input);
  const auto output_size = ResourceDimensions(resources.scaling_output);
  if (!render_size.IsValid() || !output_size.IsValid()) return false;

  auto* output = NativeResource(resources.scaling_output);
  if (output == nullptr) return false;
  if ((output->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) return false;

  if (!state.constants.has_value()) return false;
  const auto& constants = *state.constants;
  if (!IsValidSlFloat(constants.jitterOffset.x)
      || !IsValidSlFloat(constants.jitterOffset.y)
      || !IsValidSlFloat(constants.mvecScale.x)
      || !IsValidSlFloat(constants.mvecScale.y)
      || !IsValidSlFloat(constants.cameraNear)
      || !IsValidSlFloat(constants.cameraFar)
      || !IsValidSlFloat(constants.cameraFOV)
      || constants.cameraFOV <= 0.0f) {
    return false;
  }

  const auto* options = DlssOptions(state);
  if (options != nullptr && options->mode == sl::DLSSMode::eOff) return false;
  if (!IsUsableOptionalResource(resources.exposure) && !UseAutoExposure(state)) return false;
  return true;
}

inline bool NeedsTransition(D3D12_RESOURCE_STATES current, D3D12_RESOURCE_STATES required) {
  if (required == D3D12_RESOURCE_STATE_COMMON) return current != required;
  return (current & required) != required;
}

inline void TransitionResource(
    ID3D12GraphicsCommandList* command_list,
    const CapturedResource& captured,
    D3D12_RESOURCE_STATES required,
    std::vector<RestoreTransition>& restore) {
  auto* resource = NativeResource(captured);
  if (resource == nullptr || !IsKnownState(captured)) return;

  const auto original = static_cast<D3D12_RESOURCE_STATES>(captured.resource->state);
  if (!NeedsTransition(original, required)) return;

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = original;
  barrier.Transition.StateAfter = required;
  command_list->ResourceBarrier(1u, &barrier);
  restore.push_back({resource, original, required});
}

inline void RestoreResourceStates(
    ID3D12GraphicsCommandList* command_list,
    const std::vector<RestoreTransition>& restore) {
  for (auto it = restore.rbegin(); it != restore.rend(); ++it) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = it->resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = it->ffx_state;
    barrier.Transition.StateAfter = it->original;
    command_list->ResourceBarrier(1u, &barrier);
  }
}

inline FfxApiResource ReadResource(const CapturedResource& captured) {
  return ffxApiGetResourceDX12(NativeResource(captured), FFX_API_RESOURCE_STATE_COMPUTE_READ);
}

inline FfxApiResource OptionalReadResource(const CapturedResource& captured) {
  if (!IsUsableOptionalResource(captured) || HasUnsupportedOffset(captured)) return {};
  return ReadResource(captured);
}

inline FfxApiResource OutputResource(const CapturedResource& captured) {
  return ffxApiGetResourceDX12(
      NativeResource(captured),
      FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,
      FFX_API_RESOURCE_USAGE_UAV);
}

inline std::optional<uint32_t> QualityMode(sl::DLSSMode mode) {
  switch (mode) {
    case sl::DLSSMode::eDLAA:
      return FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
    case sl::DLSSMode::eMaxQuality:
    case sl::DLSSMode::eUltraQuality:
      return FFX_UPSCALE_QUALITY_MODE_QUALITY;
    case sl::DLSSMode::eBalanced:
      return FFX_UPSCALE_QUALITY_MODE_BALANCED;
    case sl::DLSSMode::eMaxPerformance:
      return FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
    case sl::DLSSMode::eUltraPerformance:
      return FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
    default:
      return std::nullopt;
  }
}

inline bool BridgePreflight(
    sl::Feature feature,
    const sl::FrameToken&,
    const sl::BaseStructure**,
    uint32_t,
    sl::CommandBuffer* command_buffer,
    const FrameState& state) {
  if (feature != sl::kFeatureDLSS || command_buffer == nullptr) return false;

  std::scoped_lock lock(backend_mutex);
  if (!initialized || device == nullptr || !ffx::GetRuntime().IsReady()) return false;
  if (!RequiredResourcesReady(state)) return false;

  const auto render_size = ResourceDimensions(state.resources.scaling_input);
  const auto output_size = ResourceDimensions(state.resources.scaling_output);
  const auto flags = BuildCreateFlags(state);
  return CreateContextLocked(state.viewport, render_size, output_size, flags);
}

inline sl::Result BridgeEvaluate(
    sl::Feature feature,
    const sl::FrameToken&,
    const sl::BaseStructure**,
    uint32_t,
    sl::CommandBuffer* command_buffer,
    const FrameState& state) {
  if (feature != sl::kFeatureDLSS || command_buffer == nullptr) {
    return sl::Result::eErrorMissingInputParameter;
  }

  auto* command_list = reinterpret_cast<ID3D12GraphicsCommandList*>(command_buffer);
  if (command_list == nullptr) return sl::Result::eErrorD3DAPI;

  std::scoped_lock lock(backend_mutex);
  const auto record_it = contexts.find(state.viewport);
  if (record_it == contexts.end() || record_it->second.context == nullptr) {
    return sl::Result::eErrorNotInitialized;
  }

  const auto& runtime = ffx::GetRuntime();
  if (!runtime.IsReady()) return sl::Result::eErrorNotInitialized;

  const auto& resources = state.resources;
  const auto& constants = *state.constants;
  const auto render_size = ResourceDimensions(resources.scaling_input);
  const auto output_size = ResourceDimensions(resources.scaling_output);

  std::vector<RestoreTransition> restore;
  restore.reserve(8u);

  constexpr auto kReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  TransitionResource(command_list, resources.scaling_input, kReadState, restore);
  TransitionResource(command_list, resources.depth, kReadState, restore);
  TransitionResource(command_list, resources.motion_vectors, kReadState, restore);
  if (IsUsableOptionalResource(resources.exposure)) TransitionResource(command_list, resources.exposure, kReadState, restore);
  if (IsUsableOptionalResource(resources.reactive_mask)) TransitionResource(command_list, resources.reactive_mask, kReadState, restore);
  if (IsUsableOptionalResource(resources.transparency_composition_mask)) TransitionResource(command_list, resources.transparency_composition_mask, kReadState, restore);
  TransitionResource(command_list, resources.scaling_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, restore);

  ffxDispatchDescUpscale dispatch{};
  dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  dispatch.commandList = command_list;
  dispatch.color = ReadResource(resources.scaling_input);
  dispatch.depth = ReadResource(resources.depth);
  dispatch.motionVectors = ReadResource(resources.motion_vectors);
  dispatch.exposure = UseAutoExposure(state) ? FfxApiResource{} : OptionalReadResource(resources.exposure);
  dispatch.reactive = OptionalReadResource(resources.reactive_mask);
  dispatch.transparencyAndComposition = OptionalReadResource(resources.transparency_composition_mask);
  dispatch.output = OutputResource(resources.scaling_output);
  dispatch.jitterOffset = {constants.jitterOffset.x, constants.jitterOffset.y};

  // Streamline mvecScale normalizes the game's raw vectors to [-1,1]. FSR
  // expects screen-space pixel vectors, so apply the render dimensions after
  // the Streamline normalization scale.
  dispatch.motionVectorScale = {
      constants.mvecScale.x * static_cast<float>(render_size.width),
      constants.mvecScale.y * static_cast<float>(render_size.height)};

  dispatch.renderSize = {render_size.width, render_size.height};
  dispatch.upscaleSize = {output_size.width, output_size.height};
  dispatch.enableSharpening = config.enable_sharpening;
  dispatch.sharpness = std::clamp(config.sharpness, 0.0f, 1.0f);
  dispatch.frameTimeDelta = std::max(config.frame_time_delta_ms, 0.001f);
  dispatch.preExposure = EffectivePreExposure(state);
  dispatch.reset = constants.reset == sl::Boolean::eTrue;
  dispatch.cameraNear = constants.cameraNear;
  dispatch.cameraFar = constants.cameraFar;
  dispatch.cameraFovAngleVertical = VerticalFov(constants);
  dispatch.viewSpaceToMetersFactor = std::max(config.view_space_to_meters, 0.0001f);
  dispatch.flags = 0u;
  if (config.input_color_space == InputColorSpace::kSRGB) {
    dispatch.flags |= FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
  } else if (config.input_color_space == InputColorSpace::kPQ) {
    dispatch.flags |= FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ;
  }

  const auto ffx_result = runtime.dispatch(&record_it->second.context, &dispatch.header);

  D3D12_RESOURCE_BARRIER uav_barrier{};
  uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav_barrier.UAV.pResource = NativeResource(resources.scaling_output);
  command_list->ResourceBarrier(1u, &uav_barrier);

  RestoreResourceStates(command_list, restore);
  return MapFfxResult(ffx_result);
}

}  // namespace internal

inline bool Initialize(
    ID3D12Device* device,
    const BackendConfig& config = {},
    const std::filesystem::path& runtime_directory = L".") {
  SetBackendReady(false);
  SetBackendCallbacks(nullptr, nullptr);

  if (device == nullptr || !internal::IsAmdDevice(device) || !ffx::Load(runtime_directory)) return false;

  std::scoped_lock lock(internal::backend_mutex);
  internal::DestroyAllContextsLocked();
  if (internal::device != nullptr) internal::device->Release();

  internal::device = device;
  internal::device->AddRef();
  internal::config = config;
  internal::initialized = internal::ProviderAvailableLocked();

  if (!internal::initialized) {
    internal::device->Release();
    internal::device = nullptr;
    ffx::Unload();
    return false;
  }

  SetBackendCallbacks(&internal::BridgePreflight, &internal::BridgeEvaluate);
  SetBackendReady(true);
  return true;
}

inline void Shutdown() {
  SetBackendReady(false);
  SetBackendCallbacks(nullptr, nullptr);
  ResetState();

  std::scoped_lock lock(internal::backend_mutex);
  internal::DestroyAllContextsLocked();
  internal::initialized = false;
  if (internal::device != nullptr) {
    internal::device->Release();
    internal::device = nullptr;
  }
  ffx::Unload();
}

inline void UpdateConfig(const BackendConfig& config) {
  std::scoped_lock lock(internal::backend_mutex);
  internal::config = config;
  internal::DestroyAllContextsLocked();
}

inline sl::Result SetDlssOptions(
    const sl::ViewportHandle& viewport,
    const sl::DLSSOptions& options) {
  CaptureDlssOptions(viewport, options);
  std::scoped_lock lock(internal::backend_mutex);
  const auto it = internal::contexts.find(static_cast<uint32_t>(viewport));
  if (it != internal::contexts.end()) internal::DestroyContextLocked(it->second);
  return internal::initialized ? sl::Result::eOk : sl::Result::eErrorNotInitialized;
}

inline sl::Result GetOptimalSettings(
    const sl::DLSSOptions& options,
    sl::DLSSOptimalSettings& settings) {
  if (options.outputWidth == sl::INVALID_UINT || options.outputHeight == sl::INVALID_UINT) {
    return sl::Result::eErrorMissingInputParameter;
  }

  const auto quality = internal::QualityMode(options.mode);
  if (!quality.has_value()) return sl::Result::eErrorInvalidParameter;

  std::scoped_lock lock(internal::backend_mutex);
  if (!internal::initialized || !ffx::GetRuntime().IsReady()) return sl::Result::eErrorNotInitialized;

  uint32_t render_width = 0u;
  uint32_t render_height = 0u;
  ffxQueryDescUpscaleGetRenderResolutionFromQualityMode query{};
  query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
  query.displayWidth = options.outputWidth;
  query.displayHeight = options.outputHeight;
  query.qualityMode = *quality;
  query.pOutRenderWidth = &render_width;
  query.pOutRenderHeight = &render_height;

  const auto result = ffx::GetRuntime().query(nullptr, &query.header);
  if (result != FFX_API_RETURN_OK || render_width == 0u || render_height == 0u) {
    return internal::MapFfxResult(result);
  }

  settings.optimalRenderWidth = render_width;
  settings.optimalRenderHeight = render_height;
  settings.optimalSharpness = 0.0f;
  // PoC compatibility: use an exact recommended range until provider-specific
  // dynamic-resolution limits are queried and mapped.
  settings.renderWidthMin = render_width;
  settings.renderHeightMin = render_height;
  settings.renderWidthMax = render_width;
  settings.renderHeightMax = render_height;
  return sl::Result::eOk;
}

inline sl::Result GetState(const sl::ViewportHandle&, sl::DLSSState& state) {
  std::scoped_lock lock(internal::backend_mutex);
  if (!internal::initialized) return sl::Result::eErrorNotInitialized;
  state.estimatedVRAMUsageInBytes = 0u;
  return sl::Result::eOk;
}

[[nodiscard]] inline bool IsInitialized() {
  std::scoped_lock lock(internal::backend_mutex);
  return internal::initialized && IsBackendAvailable();
}

#else

inline bool Initialize(ID3D12Device*, const BackendConfig& = {}, const std::filesystem::path& = L".") { return false; }
inline void Shutdown() {}
inline void UpdateConfig(const BackendConfig&) {}
inline sl::Result SetDlssOptions(const sl::ViewportHandle&, const sl::DLSSOptions&) { return sl::Result::eErrorFeatureNotSupported; }
inline sl::Result GetOptimalSettings(const sl::DLSSOptions&, sl::DLSSOptimalSettings&) { return sl::Result::eErrorFeatureNotSupported; }
inline sl::Result GetState(const sl::ViewportHandle&, sl::DLSSState&) { return sl::Result::eErrorFeatureNotSupported; }
[[nodiscard]] inline bool IsInitialized() { return false; }

#endif  // RENODX_AMD_HAS_FFX_UPSCALE_DX12

}  // namespace renodx::utils::dlss::amd_bridge::ffx_dx12
