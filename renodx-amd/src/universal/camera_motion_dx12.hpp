#pragma once

#include "universal_frame.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>

namespace renodx::universal::camera_motion_dx12 {

namespace internal {

struct MotionTarget {
  ID3D12Resource* resource = nullptr;
  Extent2D extent{};
};

// Shader-visible descriptors cannot be rewritten while an in-flight command
// list may still reference them. Keep each depth/motion descriptor pair
// immutable for its lifetime and cache new pairs when resources rotate.
struct DescriptorSet {
  ID3D12DescriptorHeap* heap = nullptr;
  ID3D12Resource* depth = nullptr;
  ID3D12Resource* motion = nullptr;
};

struct ShaderConstants {
  float current_to_previous_clip[16]{};
  uint32_t render_width = 0u;
  uint32_t render_height = 0u;
  uint32_t padding[2]{};
};
static_assert(sizeof(ShaderConstants) == 20u * sizeof(uint32_t));

inline ID3D12Device* device = nullptr;
inline ID3D12RootSignature* root_signature = nullptr;
inline ID3D12PipelineState* pipeline_state = nullptr;
inline HMODULE d3d12_module = nullptr;
inline uint32_t descriptor_size = 0u;
inline std::unordered_map<uint32_t, MotionTarget> targets;
inline std::vector<MotionTarget> retired_targets;
inline std::vector<DescriptorSet> descriptor_sets;

inline void ReleaseTarget(MotionTarget& target) {
  if (target.resource != nullptr) target.resource->Release();
  target = {};
}

inline void ReleaseDescriptorSet(DescriptorSet& set) {
  if (set.heap != nullptr) set.heap->Release();
  if (set.depth != nullptr) set.depth->Release();
  if (set.motion != nullptr) set.motion->Release();
  set = {};
}

inline std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) return {};
  const auto end = stream.tellg();
  if (end <= 0) return {};
  std::vector<uint8_t> bytes(static_cast<size_t>(end));
  stream.seekg(0, std::ios::beg);
  if (!stream.read(reinterpret_cast<char*>(bytes.data()), end)) return {};
  return bytes;
}

// Return only SRV formats that are valid for the resource format we actually
// received. Typed DSV-only resources intentionally fail closed; games normally
// expose shader-readable depth through a typeless or already-readable format.
inline DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
      return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
      return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

inline bool SupportsFormat(
    DXGI_FORMAT format,
    D3D12_FORMAT_SUPPORT1 required1,
    D3D12_FORMAT_SUPPORT2 required2 = D3D12_FORMAT_SUPPORT2_NONE) {
  if (device == nullptr || format == DXGI_FORMAT_UNKNOWN) return false;
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
  support.Format = format;
  if (FAILED(device->CheckFeatureSupport(
          D3D12_FEATURE_FORMAT_SUPPORT,
          &support,
          sizeof(support)))) {
    return false;
  }
  return (support.Support1 & required1) == required1
      && (support.Support2 & required2) == required2;
}

inline bool FrameCompatible(const UniversalFrame& frame, ID3D12Resource* depth) {
  if (device == nullptr || root_signature == nullptr || pipeline_state == nullptr
      || descriptor_size == 0u || depth == nullptr) {
    return false;
  }
  if (!frame.render_extent.IsValid() || !frame.depth.IsValid()) return false;
  if (frame.depth.x != 0u || frame.depth.y != 0u) return false;
  if (frame.depth.extent.width != frame.render_extent.width
      || frame.depth.extent.height != frame.render_extent.height) {
    return false;
  }
  if (frame.camera.layout != MatrixLayout::kRowMajor
      || frame.camera.confidence <= 0.0f) {
    return false;
  }

  const auto desc = depth->GetDesc();
  const auto srv_format = DepthSrvFormat(desc.Format);
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
      || desc.DepthOrArraySize != 1u
      || desc.SampleDesc.Count != 1u
      || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0
      || !SupportsFormat(
          srv_format,
          static_cast<D3D12_FORMAT_SUPPORT1>(
              D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD))) {
    return false;
  }
  return desc.Width >= frame.render_extent.width
      && desc.Height >= frame.render_extent.height;
}

inline bool EnsureTarget(uint32_t viewport, Extent2D extent) {
  if (device == nullptr || !extent.IsValid()) return false;
  auto& target = targets[viewport];
  if (target.resource != nullptr
      && target.extent.width == extent.width
      && target.extent.height == extent.height) {
    return true;
  }

  // We do not own the queue fence here, so releasing a previous target during
  // a resize could free memory still referenced by an in-flight command list.
  // Retain superseded targets until backend shutdown instead.
  if (target.resource != nullptr) {
    retired_targets.push_back(target);
    target = {};
  }

  if (!SupportsFormat(
          DXGI_FORMAT_R16G16_FLOAT,
          D3D12_FORMAT_SUPPORT1_TEXTURE2D,
          D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
    return false;
  }

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  heap.CreationNodeMask = 1u;
  heap.VisibleNodeMask = 1u;

  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Alignment = 0u;
  desc.Width = extent.width;
  desc.Height = extent.height;
  desc.DepthOrArraySize = 1u;
  desc.MipLevels = 1u;
  desc.Format = DXGI_FORMAT_R16G16_FLOAT;
  desc.SampleDesc.Count = 1u;
  desc.SampleDesc.Quality = 0u;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  ID3D12Resource* resource = nullptr;
  const auto result = device->CreateCommittedResource(
      &heap,
      D3D12_HEAP_FLAG_NONE,
      &desc,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
      nullptr,
      IID_PPV_ARGS(&resource));
  if (FAILED(result) || resource == nullptr) return false;

  target.resource = resource;
  target.extent = extent;
  return true;
}

inline DescriptorSet* EnsureDescriptorSet(
    ID3D12Resource* depth,
    ID3D12Resource* motion) {
  if (device == nullptr || descriptor_size == 0u || depth == nullptr || motion == nullptr) {
    return nullptr;
  }

  for (auto& set : descriptor_sets) {
    if (set.depth == depth && set.motion == motion && set.heap != nullptr) return &set;
  }

  const auto depth_format = DepthSrvFormat(depth->GetDesc().Format);
  if (depth_format == DXGI_FORMAT_UNKNOWN) return nullptr;

  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = 2u;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  heap_desc.NodeMask = 0u;

  ID3D12DescriptorHeap* heap = nullptr;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap))) || heap == nullptr) {
    return nullptr;
  }

  auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = depth_format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MostDetailedMip = 0u;
  srv.Texture2D.MipLevels = 1u;
  srv.Texture2D.PlaneSlice = 0u;
  srv.Texture2D.ResourceMinLODClamp = 0.0f;
  device->CreateShaderResourceView(depth, &srv, cpu);

  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R16G16_FLOAT;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  uav.Texture2D.MipSlice = 0u;
  uav.Texture2D.PlaneSlice = 0u;
  cpu.ptr += descriptor_size;
  device->CreateUnorderedAccessView(motion, nullptr, &uav, cpu);

  // Keep the resources alive as long as the immutable descriptors can be
  // referenced. This also prevents pointer reuse from aliasing an old cache key.
  depth->AddRef();
  motion->AddRef();
  descriptor_sets.push_back(DescriptorSet{
      .heap = heap,
      .depth = depth,
      .motion = motion,
  });
  return &descriptor_sets.back();
}

}  // namespace internal

inline void Shutdown() {
  for (auto& set : internal::descriptor_sets) internal::ReleaseDescriptorSet(set);
  internal::descriptor_sets.clear();

  for (auto& [_, target] : internal::targets) internal::ReleaseTarget(target);
  internal::targets.clear();
  for (auto& target : internal::retired_targets) internal::ReleaseTarget(target);
  internal::retired_targets.clear();

  if (internal::pipeline_state != nullptr) internal::pipeline_state->Release();
  if (internal::root_signature != nullptr) internal::root_signature->Release();
  if (internal::device != nullptr) internal::device->Release();
  if (internal::d3d12_module != nullptr) FreeLibrary(internal::d3d12_module);

  internal::pipeline_state = nullptr;
  internal::root_signature = nullptr;
  internal::device = nullptr;
  internal::d3d12_module = nullptr;
  internal::descriptor_size = 0u;
}

[[nodiscard]] inline bool Initialize(
    ID3D12Device* candidate,
    const std::filesystem::path& shader_path) {
  Shutdown();
  if (candidate == nullptr) return false;

  const auto shader = internal::ReadFile(shader_path);
  if (shader.empty()) return false;

  HMODULE module = LoadLibraryW(L"d3d12.dll");
  if (module == nullptr) return false;
  const auto serialize_root_signature = reinterpret_cast<decltype(&D3D12SerializeRootSignature)>(
      GetProcAddress(module, "D3D12SerializeRootSignature"));
  if (serialize_root_signature == nullptr) {
    FreeLibrary(module);
    return false;
  }

  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 1u;
  ranges[0].BaseShaderRegister = 0u;
  ranges[0].RegisterSpace = 0u;
  ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1u;
  ranges[1].BaseShaderRegister = 0u;
  ranges[1].RegisterSpace = 0u;
  ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

  D3D12_ROOT_PARAMETER parameters[3]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable.NumDescriptorRanges = 1u;
  parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable.NumDescriptorRanges = 1u;
  parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[2].Constants.ShaderRegister = 0u;
  parameters[2].Constants.RegisterSpace = 0u;
  parameters[2].Constants.Num32BitValues = 20u;
  parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.NumParameters = 3u;
  root_desc.pParameters = parameters;
  root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  ID3DBlob* serialized = nullptr;
  ID3DBlob* errors = nullptr;
  const auto serialize_result = serialize_root_signature(
      &root_desc,
      D3D_ROOT_SIGNATURE_VERSION_1,
      &serialized,
      &errors);
  if (errors != nullptr) errors->Release();
  if (FAILED(serialize_result) || serialized == nullptr) {
    if (serialized != nullptr) serialized->Release();
    FreeLibrary(module);
    return false;
  }

  ID3D12RootSignature* root_signature = nullptr;
  const auto root_result = candidate->CreateRootSignature(
      0u,
      serialized->GetBufferPointer(),
      serialized->GetBufferSize(),
      IID_PPV_ARGS(&root_signature));
  serialized->Release();
  if (FAILED(root_result) || root_signature == nullptr) {
    FreeLibrary(module);
    return false;
  }

  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
  pipeline_desc.pRootSignature = root_signature;
  pipeline_desc.CS = {shader.data(), shader.size()};

  ID3D12PipelineState* pipeline_state = nullptr;
  const auto pipeline_result = candidate->CreateComputePipelineState(
      &pipeline_desc,
      IID_PPV_ARGS(&pipeline_state));
  if (FAILED(pipeline_result) || pipeline_state == nullptr) {
    root_signature->Release();
    FreeLibrary(module);
    return false;
  }

  const auto descriptor_size = candidate->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  if (descriptor_size == 0u) {
    pipeline_state->Release();
    root_signature->Release();
    FreeLibrary(module);
    return false;
  }

  candidate->AddRef();
  internal::device = candidate;
  internal::root_signature = root_signature;
  internal::pipeline_state = pipeline_state;
  internal::d3d12_module = module;
  internal::descriptor_size = descriptor_size;
  return true;
}

[[nodiscard]] inline bool IsReady() noexcept {
  return internal::device != nullptr
      && internal::root_signature != nullptr
      && internal::pipeline_state != nullptr
      && internal::descriptor_size != 0u;
}

[[nodiscard]] inline bool Prepare(
    uint32_t viewport,
    const UniversalFrame& frame,
    ID3D12Resource* depth) {
  if (!internal::FrameCompatible(frame, depth)
      || !internal::EnsureTarget(viewport, frame.render_extent)) {
    return false;
  }

  const auto target_it = internal::targets.find(viewport);
  return target_it != internal::targets.end()
      && target_it->second.resource != nullptr
      && internal::EnsureDescriptorSet(depth, target_it->second.resource) != nullptr;
}

[[nodiscard]] inline ID3D12Resource* Dispatch(
    ID3D12GraphicsCommandList* command_list,
    uint32_t viewport,
    const UniversalFrame& frame,
    ID3D12Resource* depth) {
  if (command_list == nullptr || !Prepare(viewport, frame, depth)) return nullptr;

  const auto target_it = internal::targets.find(viewport);
  if (target_it == internal::targets.end() || target_it->second.resource == nullptr) return nullptr;
  auto* motion = target_it->second.resource;
  auto* descriptors = internal::EnsureDescriptorSet(depth, motion);
  if (descriptors == nullptr || descriptors->heap == nullptr) return nullptr;

  D3D12_RESOURCE_BARRIER to_uav{};
  to_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  to_uav.Transition.pResource = motion;
  to_uav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  to_uav.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  to_uav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  command_list->ResourceBarrier(1u, &to_uav);

  ID3D12DescriptorHeap* heaps[] = {descriptors->heap};
  command_list->SetDescriptorHeaps(1u, heaps);
  command_list->SetComputeRootSignature(internal::root_signature);
  command_list->SetPipelineState(internal::pipeline_state);

  auto gpu = descriptors->heap->GetGPUDescriptorHandleForHeapStart();
  command_list->SetComputeRootDescriptorTable(0u, gpu);
  gpu.ptr += internal::descriptor_size;
  command_list->SetComputeRootDescriptorTable(1u, gpu);

  internal::ShaderConstants constants{};
  std::copy(
      frame.camera.clip_to_previous_clip.begin(),
      frame.camera.clip_to_previous_clip.end(),
      constants.current_to_previous_clip);
  constants.render_width = frame.render_extent.width;
  constants.render_height = frame.render_extent.height;
  command_list->SetComputeRoot32BitConstants(2u, 20u, &constants, 0u);
  command_list->Dispatch(
      (frame.render_extent.width + 15u) / 16u,
      (frame.render_extent.height + 15u) / 16u,
      1u);

  D3D12_RESOURCE_BARRIER uav_barrier{};
  uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav_barrier.UAV.pResource = motion;
  command_list->ResourceBarrier(1u, &uav_barrier);

  D3D12_RESOURCE_BARRIER to_read{};
  to_read.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  to_read.Transition.pResource = motion;
  to_read.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  to_read.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  to_read.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  command_list->ResourceBarrier(1u, &to_read);
  return motion;
}

}  // namespace renodx::universal::camera_motion_dx12
