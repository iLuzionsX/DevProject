#!/usr/bin/env python3
"""Materialize the RenoDX AMD bridge into a pinned RenoDX worktree.

This replaces the original illustrative patch files with deterministic source
transformations. Every transformation must match exactly once; upstream drift
therefore fails CI before compilation instead of silently producing a partial
integration.
"""

from __future__ import annotations

import argparse
import re
import shutil
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one anchor, found {count}")
    return text.replace(old, new, 1)


def regex_once(text: str, pattern: str, replacement: str, label: str) -> str:
    new_text, count = re.subn(pattern, replacement, text, count=1, flags=re.DOTALL)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one regex match, found {count}")
    return new_text


def patch_amd_bridge(path: Path) -> None:
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        "inline void ApplyTags(FrameResources& resources, const sl::ResourceTag* tags, uint32_t num_tags) {",
        "inline void ApplyTags(\n"
        "    FrameResources& resources,\n"
        "    const sl::ResourceTag* tags,\n"
        "    uint32_t num_tags,\n"
        "    bool allow_only_valid_now = false) {",
        "amd_bridge ApplyTags signature",
    )

    text = replace_once(
        text,
        "    auto* slot = FindResourceSlot(resources, tag.type);\n"
        "    if (slot == nullptr) continue;\n\n"
        "    if (tag.resource == nullptr || tag.resource->native == nullptr) {",
        "    auto* slot = FindResourceSlot(resources, tag.type);\n"
        "    if (slot == nullptr) continue;\n\n"
        "    // eOnlyValidNow resources may be changed or reused immediately after\n"
        "    // slSetTag/slSetTagForFrame returns. Only consume them when supplied\n"
        "    // directly as part of the evaluate call.\n"
        "    if (tag.lifecycle == sl::eOnlyValidNow && !allow_only_valid_now) {\n"
        "      slot->resource.reset();\n"
        "      slot->extent.reset();\n"
        "      continue;\n"
        "    }\n\n"
        "    if (tag.resource == nullptr || tag.resource->native == nullptr) {",
        "amd_bridge transient lifecycle",
    )

    text = replace_once(
        text,
        "        ApplyTags(state.resources, reinterpret_cast<const sl::ResourceTag*>(current), 1u);",
        "        ApplyTags(\n"
        "            state.resources,\n"
        "            reinterpret_cast<const sl::ResourceTag*>(current),\n"
        "            1u,\n"
        "            true);",
        "amd_bridge evaluate-local tags",
    )

    text = replace_once(
        text,
        "inline void ResetState() {\n"
        "  std::unique_lock lock(internal::state_mutex);\n"
        "  internal::states.clear();\n"
        "  internal::next_sequence = 1u;\n"
        "}\n",
        "inline void ResetState() {\n"
        "  std::unique_lock lock(internal::state_mutex);\n"
        "  internal::states.clear();\n"
        "  internal::next_sequence = 1u;\n"
        "}\n\n"
        "inline void ConsumeFrameState(uint32_t frame_index, uint32_t viewport) {\n"
        "  std::unique_lock lock(internal::state_mutex);\n"
        "  internal::states.erase(internal::FrameKey{frame_index, viewport});\n\n"
        "  // Deprecated slSetTag is viewport-global, but its resources still have\n"
        "  // finite lifetimes. Clear only resources after evaluation; DLSS options\n"
        "  // remain persistent for the viewport.\n"
        "  const auto legacy = internal::states.find(\n"
        "      internal::FrameKey{kLegacyFrameIndex, viewport});\n"
        "  if (legacy != internal::states.end()) {\n"
        "    legacy->second.state.resources = {};\n"
        "  }\n"
        "}\n",
        "amd_bridge ConsumeFrameState",
    )

    text = replace_once(
        text,
        "  *result = evaluate(feature, frame, inputs, num_inputs, command_buffer, *state);\n"
        "  return true;",
        "  *result = evaluate(feature, frame, inputs, num_inputs, command_buffer, *state);\n"
        "  ConsumeFrameState(state->frame_index, state->viewport);\n"
        "  return true;",
        "amd_bridge consume after evaluate",
    )

    path.write_text(text, encoding="utf-8", newline="\n")


def patch_ffx_backend(path: Path) -> None:
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        "#include <algorithm>\n#include <cmath>",
        "#include <algorithm>\n#include <chrono>\n#include <cmath>",
        "ffx backend chrono include",
    )

    text = replace_once(
        text,
        "struct ContextRecord {\n"
        "  ffxContext context = nullptr;\n"
        "  Dimensions max_render_size{};\n"
        "  Dimensions max_upscale_size{};\n"
        "  uint32_t create_flags = 0u;\n"
        "};",
        "struct ContextRecord {\n"
        "  ffxContext context = nullptr;\n"
        "  Dimensions max_render_size{};\n"
        "  Dimensions max_upscale_size{};\n"
        "  uint32_t create_flags = 0u;\n"
        "  std::optional<std::chrono::steady_clock::time_point> last_dispatch_time;\n"
        "};",
        "ffx backend context timing",
    )

    output_resource = (
        "inline FfxApiResource OutputResource(const CapturedResource& captured) {\n"
        "  return ffxApiGetResourceDX12(\n"
        "      NativeResource(captured),\n"
        "      FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,\n"
        "      FFX_API_RESOURCE_USAGE_UAV);\n"
        "}\n"
    )
    text = replace_once(
        text,
        output_resource,
        output_resource
        + "\ninline float MeasureFrameTimeDelta(ContextRecord& record) {\n"
        "  const auto now = std::chrono::steady_clock::now();\n"
        "  float delta_ms = std::max(config.frame_time_delta_ms, 0.001f);\n"
        "  if (record.last_dispatch_time.has_value()) {\n"
        "    delta_ms = std::chrono::duration<float, std::milli>(\n"
        "        now - *record.last_dispatch_time).count();\n"
        "    if (!std::isfinite(delta_ms) || delta_ms <= 0.0f) {\n"
        "      delta_ms = std::max(config.frame_time_delta_ms, 0.001f);\n"
        "    }\n"
        "  }\n"
        "  record.last_dispatch_time = now;\n"
        "  return delta_ms;\n"
        "}\n",
        "ffx backend measured frame delta",
    )

    quality_mode = (
        "inline std::optional<uint32_t> QualityMode(sl::DLSSMode mode) {\n"
        "  switch (mode) {\n"
        "    case sl::DLSSMode::eDLAA:\n"
        "      return FFX_UPSCALE_QUALITY_MODE_NATIVEAA;\n"
        "    case sl::DLSSMode::eMaxQuality:\n"
        "    case sl::DLSSMode::eUltraQuality:\n"
        "      return FFX_UPSCALE_QUALITY_MODE_QUALITY;\n"
        "    case sl::DLSSMode::eBalanced:\n"
        "      return FFX_UPSCALE_QUALITY_MODE_BALANCED;\n"
        "    case sl::DLSSMode::eMaxPerformance:\n"
        "      return FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;\n"
        "    case sl::DLSSMode::eUltraPerformance:\n"
        "      return FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;\n"
        "    default:\n"
        "      return std::nullopt;\n"
        "  }\n"
        "}\n"
    )
    text = replace_once(
        text,
        quality_mode,
        quality_mode
        + "\ninline std::optional<float> QualityRatio(sl::DLSSMode mode) {\n"
        "  switch (mode) {\n"
        "    case sl::DLSSMode::eDLAA:\n"
        "      return 1.0f;\n"
        "    case sl::DLSSMode::eMaxQuality:\n"
        "    case sl::DLSSMode::eUltraQuality:\n"
        "      return 1.5f;\n"
        "    case sl::DLSSMode::eBalanced:\n"
        "      return 1.7f;\n"
        "    case sl::DLSSMode::eMaxPerformance:\n"
        "      return 2.0f;\n"
        "    case sl::DLSSMode::eUltraPerformance:\n"
        "      return 3.0f;\n"
        "    default:\n"
        "      return std::nullopt;\n"
        "  }\n"
        "}\n",
        "ffx backend quality ratios",
    )

    text = replace_once(
        text,
        "  const auto render_size = ResourceDimensions(resources.scaling_input);\n"
        "  const auto output_size = ResourceDimensions(resources.scaling_output);\n\n"
        "  std::vector<RestoreTransition> restore;",
        "  const auto render_size = ResourceDimensions(resources.scaling_input);\n"
        "  const auto output_size = ResourceDimensions(resources.scaling_output);\n"
        "  const auto motion_vector_size = ResourceDimensions(resources.motion_vectors);\n\n"
        "  if (!motion_vector_size.IsValid()) {\n"
        "    return sl::Result::eErrorMissingInputParameter;\n"
        "  }\n\n"
        "  std::vector<RestoreTransition> restore;",
        "ffx backend MV domain dimensions",
    )

    text = replace_once(
        text,
        "  // Streamline mvecScale normalizes the game's raw vectors to [-1,1]. FSR\n"
        "  // expects screen-space pixel vectors, so apply the render dimensions after\n"
        "  // the Streamline normalization scale.\n"
        "  dispatch.motionVectorScale = {\n"
        "      constants.mvecScale.x * static_cast<float>(render_size.width),\n"
        "      constants.mvecScale.y * static_cast<float>(render_size.height)};",
        "  // Convert Streamline's normalization scale in the actual motion-vector\n"
        "  // resource domain. This handles both render-res and display-res MVs.\n"
        "  dispatch.motionVectorScale = {\n"
        "      constants.mvecScale.x * static_cast<float>(motion_vector_size.width),\n"
        "      constants.mvecScale.y * static_cast<float>(motion_vector_size.height)};",
        "ffx backend MV scale",
    )

    text = replace_once(
        text,
        "  dispatch.frameTimeDelta = std::max(config.frame_time_delta_ms, 0.001f);",
        "  dispatch.frameTimeDelta = MeasureFrameTimeDelta(record_it->second);",
        "ffx backend frame delta dispatch",
    )

    text = regex_once(
        text,
        r"  const auto quality = internal::QualityMode\(options\.mode\);\n"
        r"  if \(!quality\.has_value\(\)\) return sl::Result::eErrorInvalidParameter;\n\n"
        r"  std::scoped_lock lock\(internal::backend_mutex\);\n"
        r"  if \(!internal::initialized \|\| !ffx::GetRuntime\(\)\.IsReady\(\)\) return sl::Result::eErrorNotInitialized;\n\n"
        r"  uint32_t render_width = 0u;\n"
        r"  uint32_t render_height = 0u;\n"
        r"  ffxQueryDescUpscaleGetRenderResolutionFromQualityMode query\{\};\n"
        r"  query\.header\.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;\n"
        r"  query\.displayWidth = options\.outputWidth;\n"
        r"  query\.displayHeight = options\.outputHeight;\n"
        r"  query\.qualityMode = \*quality;\n"
        r"  query\.pOutRenderWidth = &render_width;\n"
        r"  query\.pOutRenderHeight = &render_height;\n\n"
        r"  const auto result = ffx::GetRuntime\(\)\.query\(nullptr, &query\.header\);\n"
        r"  if \(result != FFX_API_RETURN_OK \|\| render_width == 0u \|\| render_height == 0u\) \{\n"
        r"    return internal::MapFfxResult\(result\);\n"
        r"  \}",
        "  const auto ratio = internal::QualityRatio(options.mode);\n"
        "  if (!ratio.has_value() || *ratio <= 0.0f) return sl::Result::eErrorInvalidParameter;\n\n"
        "  // Match the FidelityFX provider's public quality-mode helper: divide\n"
        "  // each display dimension by the fixed ratio and truncate to uint32_t.\n"
        "  const auto render_width = static_cast<uint32_t>(\n"
        "      static_cast<float>(options.outputWidth) / *ratio);\n"
        "  const auto render_height = static_cast<uint32_t>(\n"
        "      static_cast<float>(options.outputHeight) / *ratio);\n"
        "  if (render_width == 0u || render_height == 0u) {\n"
        "    return sl::Result::eErrorInvalidParameter;\n"
        "  }",
        "ffx backend pre-device optimal settings",
    )

    path.write_text(text, encoding="utf-8", newline="\n")


def patch_streamline(path: Path) -> None:
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        '#include "../resource.hpp"\n#include "./DXGIFactoryWrapper.hpp"',
        '#include "../resource.hpp"\n'
        '#include "./amd_bridge.hpp"\n'
        '#include "./ffx_upscale_backend_dx12.hpp"\n'
        '#include "./ffx_support_probe_dx12.hpp"\n'
        '#include "./DXGIFactoryWrapper.hpp"',
        "streamline bridge includes",
    )

    text = replace_once(
        text,
        "static decltype(&slInit) Real_slInit = nullptr;\n"
        "static decltype(&slGetNativeInterface) Real_slGetNativeInterface = nullptr;",
        "static decltype(&slInit) Real_slInit = nullptr;\n"
        "static decltype(&slShutdown) Real_slShutdown = nullptr;\n"
        "static decltype(&slIsFeatureSupported) Real_slIsFeatureSupported = nullptr;\n"
        "static decltype(&slIsFeatureLoaded) Real_slIsFeatureLoaded = nullptr;\n"
        "static decltype(&slSetConstants) Real_slSetConstants = nullptr;\n"
        "static decltype(&slGetNativeInterface) Real_slGetNativeInterface = nullptr;",
        "streamline real function declarations",
    )

    # Insert lifecycle/support hooks immediately before slEvaluateFeature, after
    # all of the standard Streamline initialization machinery is declared.
    evaluate_marker = "static decltype(&slEvaluateFeature) Real_slEvaluateFeature = nullptr;"
    hook_block = (
        "SL_API sl::Result Hooked_slShutdown() {\n"
        "  renodx::utils::dlss::amd_bridge::ffx_dx12::Shutdown();\n"
        "  return Real_slShutdown();\n"
        "}\n\n"
        "static sl::Result Hooked_slIsFeatureSupported(\n"
        "    sl::Feature feature, const sl::AdapterInfo& adapterInfo) {\n"
        "  if (feature == sl::kFeatureDLSS) {\n"
        "    if (renodx::utils::dlss::amd_bridge::IsBackendAvailable()\n"
        "        || renodx::utils::dlss::amd_bridge::ffx_dx12::ProbeAdapterSupport(adapterInfo)) {\n"
        "      return sl::Result::eOk;\n"
        "    }\n"
        "  }\n"
        "  return Real_slIsFeatureSupported(feature, adapterInfo);\n"
        "}\n\n"
        "static sl::Result Hooked_slIsFeatureLoaded(sl::Feature feature, bool& loaded) {\n"
        "  if (feature == sl::kFeatureDLSS\n"
        "      && renodx::utils::dlss::amd_bridge::IsBackendAvailable()) {\n"
        "    loaded = true;\n"
        "    return sl::Result::eOk;\n"
        "  }\n"
        "  return Real_slIsFeatureLoaded(feature, loaded);\n"
        "}\n\n"
        "static sl::Result Hooked_slSetConstants(\n"
        "    const sl::Constants& values,\n"
        "    const sl::FrameToken& frame,\n"
        "    const sl::ViewportHandle& viewport) {\n"
        "  if (renodx::utils::dlss::amd_bridge::IsBackendAvailable()) {\n"
        "    renodx::utils::dlss::amd_bridge::CaptureConstants(values, frame, viewport);\n"
        "  }\n"
        "  return Real_slSetConstants(values, frame, viewport);\n"
        "}\n\n"
    )
    text = replace_once(
        text,
        evaluate_marker,
        hook_block + evaluate_marker,
        "streamline support/lifecycle hooks",
    )

    text = replace_once(
        text,
        "  (void)frame;\n"
        "  const bool unwrapped = utils::directx::NativeFromReShadeProxy(&cmdBuffer);\n"
        "  assert((unwrapped || cmdBuffer == nullptr) && \"slEvaluateFeature expected a ReShade proxy command buffer.\");\n"
        "  return Real_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);",
        "  const bool unwrapped = utils::directx::NativeFromReShadeProxy(&cmdBuffer);\n"
        "  assert((unwrapped || cmdBuffer == nullptr) && \"slEvaluateFeature expected a ReShade proxy command buffer.\");\n\n"
        "  sl::Result alternate_result = sl::Result::eOk;\n"
        "  if (renodx::utils::dlss::amd_bridge::TryEvaluate(\n"
        "          feature, frame, inputs, numInputs, cmdBuffer, &alternate_result)) {\n"
        "    // TryEvaluate returns true only after AMD preflight transfers ownership.\n"
        "    // Never call NVIDIA Streamline after the alternate backend may record work.\n"
        "    return alternate_result;\n"
        "  }\n\n"
        "  return Real_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);",
        "streamline evaluate routing",
    )

    dlss_pattern = (
        r'// slDLSSSetOptions\n'
        r'extern "C" decltype\(&slDLSSSetOptions\) Real_slDLSSSetOptions = nullptr;\n'
        r'extern "C" sl::Result Hooked_slDLSSSetOptions\(const sl::ViewportHandle& viewport, const sl::DLSSOptions& options\) \{.*?\n\}\n\n'
        r'// slDLSSGSetOptions'
    )
    dlss_replacement = (
        '// slDLSSSetOptions\n'
        'extern "C" decltype(&slDLSSSetOptions) Real_slDLSSSetOptions = nullptr;\n'
        'extern "C" sl::Result Hooked_slDLSSSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSOptions& options) {\n'
        '  if (renodx::utils::dlss::amd_bridge::IsBackendAvailable()) {\n'
        '    return renodx::utils::dlss::amd_bridge::ffx_dx12::SetDlssOptions(viewport, options);\n'
        '  }\n'
        '  if (renodx::utils::dlss::amd_bridge::ffx_dx12::HasPositiveAdapterProbe()) {\n'
        '    renodx::utils::dlss::amd_bridge::CaptureDlssOptions(viewport, options);\n'
        '    return sl::Result::eOk;\n'
        '  }\n'
        '  if (Real_slDLSSSetOptions == nullptr) return sl::Result::eErrorFeatureNotSupported;\n'
        '  sl::DLSSOptions options_copy = options;\n'
        '  options_copy.colorBuffersHDR = sl::Boolean::eTrue;\n'
        '  options_copy.useAutoExposure = sl::Boolean::eTrue;\n'
        '  return Real_slDLSSSetOptions(viewport, options_copy);\n'
        '}\n\n'
        'extern "C" decltype(&slDLSSGetOptimalSettings) Real_slDLSSGetOptimalSettings = nullptr;\n'
        'extern "C" sl::Result Hooked_slDLSSGetOptimalSettings(\n'
        '    const sl::DLSSOptions& options, sl::DLSSOptimalSettings& settings) {\n'
        '  if (renodx::utils::dlss::amd_bridge::IsBackendAvailable()\n'
        '      || renodx::utils::dlss::amd_bridge::ffx_dx12::HasPositiveAdapterProbe()) {\n'
        '    return renodx::utils::dlss::amd_bridge::ffx_dx12::GetOptimalSettings(options, settings);\n'
        '  }\n'
        '  if (Real_slDLSSGetOptimalSettings == nullptr) return sl::Result::eErrorFeatureNotSupported;\n'
        '  return Real_slDLSSGetOptimalSettings(options, settings);\n'
        '}\n\n'
        'extern "C" decltype(&slDLSSGetState) Real_slDLSSGetState = nullptr;\n'
        'extern "C" sl::Result Hooked_slDLSSGetState(\n'
        '    const sl::ViewportHandle& viewport, sl::DLSSState& state) {\n'
        '  if (renodx::utils::dlss::amd_bridge::IsBackendAvailable()) {\n'
        '    return renodx::utils::dlss::amd_bridge::ffx_dx12::GetState(viewport, state);\n'
        '  }\n'
        '  if (Real_slDLSSGetState == nullptr) return sl::Result::eErrorFeatureNotSupported;\n'
        '  return Real_slDLSSGetState(viewport, state);\n'
        '}\n\n'
        '// slDLSSGSetOptions'
    )
    text = regex_once(text, dlss_pattern, dlss_replacement, "streamline DLSS compatibility functions")

    text = regex_once(
        text,
        r"static decltype\(&slSetD3DDevice\) Real_slSetD3DDevice = nullptr;\n"
        r"static sl::Result Hooked_slSetD3DDevice\(void\* d3dDevice\) \{.*?\n\}\n\n"
        r"static decltype\(&slGetFeatureFunction\) Real_slGetFeatureFunction = nullptr;",
        "static decltype(&slSetD3DDevice) Real_slSetD3DDevice = nullptr;\n"
        "static sl::Result Hooked_slSetD3DDevice(void* d3dDevice) {\n"
        "  auto* native_device = static_cast<IUnknown*>(d3dDevice);\n"
        "  renodx::utils::directx::NativeFromReShadeProxy(&native_device);\n\n"
        "  ID3D12Device* d3d12_device = nullptr;\n"
        "  if (native_device != nullptr) {\n"
        "    native_device->QueryInterface(IID_PPV_ARGS(&d3d12_device));\n"
        "  }\n\n"
        "  auto ret = Real_slSetD3DDevice(native_device);\n"
        "  if (ret != sl::Result::eOk) {\n"
        "    log::e(std::format(\n"
        "               \"utils::streamline_v2::slSetD3DDevice() - Real_slSetD3DDevice failed for {:p} with result {}\",\n"
        "               (void*)d3d12_device,\n"
        "               static_cast<uint32_t>(ret))\n"
        "               .c_str());\n"
        "    if (d3d12_device != nullptr) d3d12_device->Release();\n"
        "    return ret;\n"
        "  }\n\n"
        "  if (d3d12_device != nullptr) {\n"
        "    renodx::utils::dlss::amd_bridge::ffx_dx12::Initialize(d3d12_device);\n"
        "    d3d12_device->Release();\n"
        "  }\n"
        "  return ret;\n"
        "}\n\n"
        "static decltype(&slGetFeatureFunction) Real_slGetFeatureFunction = nullptr;",
        "streamline D3D12 device gate",
    )

    feature_signature = (
        "SL_API sl::Result Hooked_slGetFeatureFunction(sl::Feature feature, const char* functionName, void*& function) {\n"
    )
    feature_override = (
        feature_signature
        + "  if (feature == sl::kFeatureDLSS) {\n"
        "    if (std::strcmp(functionName, \"slDLSSSetOptions\") == 0) {\n"
        "      void* real_function = nullptr;\n"
        "      const auto real_ret = Real_slGetFeatureFunction(feature, functionName, real_function);\n"
        "      if (real_ret == sl::Result::eOk) {\n"
        "        Real_slDLSSSetOptions = reinterpret_cast<decltype(&slDLSSSetOptions)>(real_function);\n"
        "      }\n"
        "      function = reinterpret_cast<void*>(&Hooked_slDLSSSetOptions);\n"
        "      return sl::Result::eOk;\n"
        "    }\n"
        "    if (std::strcmp(functionName, \"slDLSSGetOptimalSettings\") == 0) {\n"
        "      void* real_function = nullptr;\n"
        "      const auto real_ret = Real_slGetFeatureFunction(feature, functionName, real_function);\n"
        "      if (real_ret == sl::Result::eOk) {\n"
        "        Real_slDLSSGetOptimalSettings = reinterpret_cast<decltype(&slDLSSGetOptimalSettings)>(real_function);\n"
        "      }\n"
        "      function = reinterpret_cast<void*>(&Hooked_slDLSSGetOptimalSettings);\n"
        "      return sl::Result::eOk;\n"
        "    }\n"
        "    if (std::strcmp(functionName, \"slDLSSGetState\") == 0) {\n"
        "      void* real_function = nullptr;\n"
        "      const auto real_ret = Real_slGetFeatureFunction(feature, functionName, real_function);\n"
        "      if (real_ret == sl::Result::eOk) {\n"
        "        Real_slDLSSGetState = reinterpret_cast<decltype(&slDLSSGetState)>(real_function);\n"
        "      }\n"
        "      function = reinterpret_cast<void*>(&Hooked_slDLSSGetState);\n"
        "      return sl::Result::eOk;\n"
        "    }\n"
        "  }\n\n"
    )
    text = replace_once(text, feature_signature, feature_override, "streamline feature function wrappers")

    text = replace_once(
        text,
        "  if (rewritten_resource_count == 0u) {\n"
        "    return Real_slSetTag(viewport, tags, numTags, cmdBuffer);\n"
        "  }\n"
        "  copy_tags_until(numTags);\n\n"
        "  sl::Result result = Real_slSetTag(viewport, rewritten_tags.data(), numTags, cmdBuffer);",
        "  if (rewritten_resource_count == 0u) {\n"
        "    if (renodx::utils::dlss::amd_bridge::IsBackendAvailable()) {\n"
        "      renodx::utils::dlss::amd_bridge::CaptureTags(viewport, tags, numTags);\n"
        "    }\n"
        "    return Real_slSetTag(viewport, tags, numTags, cmdBuffer);\n"
        "  }\n"
        "  copy_tags_until(numTags);\n\n"
        "  if (renodx::utils::dlss::amd_bridge::IsBackendAvailable()) {\n"
        "    renodx::utils::dlss::amd_bridge::CaptureTags(viewport, rewritten_tags.data(), numTags);\n"
        "  }\n"
        "  sl::Result result = Real_slSetTag(viewport, rewritten_tags.data(), numTags, cmdBuffer);",
        "streamline legacy tag capture",
    )

    text = replace_once(
        text,
        "  if (rewritten_resource_count == 0u) {\n"
        "    return Real_slSetTagForFrame(frame, viewport, tags, numTags, cmdBuffer);\n"
        "  }\n"
        "  copy_tags_until(numTags);\n\n"
        "  sl::Result result = Real_slSetTagForFrame(frame, viewport, rewritten_tags.data(), numTags, cmdBuffer);",
        "  if (rewritten_resource_count == 0u) {\n"
        "    if (renodx::utils::dlss::amd_bridge::IsBackendAvailable()) {\n"
        "      renodx::utils::dlss::amd_bridge::CaptureTags(frame, viewport, tags, numTags);\n"
        "    }\n"
        "    return Real_slSetTagForFrame(frame, viewport, tags, numTags, cmdBuffer);\n"
        "  }\n"
        "  copy_tags_until(numTags);\n\n"
        "  if (renodx::utils::dlss::amd_bridge::IsBackendAvailable()) {\n"
        "    renodx::utils::dlss::amd_bridge::CaptureTags(frame, viewport, rewritten_tags.data(), numTags);\n"
        "  }\n"
        "  sl::Result result = Real_slSetTagForFrame(frame, viewport, rewritten_tags.data(), numTags, cmdBuffer);",
        "streamline frame tag capture",
    )

    text = replace_once(
        text,
        "static const std::vector<std::tuple<const char*, void**, void*>> INTERPOSER_HOOKS = {\n"
        "    {\"slInit\", reinterpret_cast<void**>(&Real_slInit), &Hooked_slInit},\n"
        "    {\"slSetTag\", reinterpret_cast<void**>(&Real_slSetTag), &Hooked_slSetTag},",
        "static const std::vector<std::tuple<const char*, void**, void*>> INTERPOSER_HOOKS = {\n"
        "    {\"slInit\", reinterpret_cast<void**>(&Real_slInit), &Hooked_slInit},\n"
        "    {\"slShutdown\", reinterpret_cast<void**>(&Real_slShutdown), &Hooked_slShutdown},\n"
        "    {\"slIsFeatureSupported\", reinterpret_cast<void**>(&Real_slIsFeatureSupported), &Hooked_slIsFeatureSupported},\n"
        "    {\"slIsFeatureLoaded\", reinterpret_cast<void**>(&Real_slIsFeatureLoaded), &Hooked_slIsFeatureLoaded},\n"
        "    {\"slSetConstants\", reinterpret_cast<void**>(&Real_slSetConstants), &Hooked_slSetConstants},\n"
        "    {\"slSetTag\", reinterpret_cast<void**>(&Real_slSetTag), &Hooked_slSetTag},",
        "streamline hook table",
    )

    path.write_text(text, encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bridge-root", required=True, type=Path)
    parser.add_argument("--renodx-root", required=True, type=Path)
    args = parser.parse_args()

    bridge_root = args.bridge_root.resolve()
    renodx_root = args.renodx_root.resolve()
    dlss_dir = renodx_root / "src" / "utils" / "dlss"

    sources = [
        "amd_bridge.hpp",
        "ffx_runtime_loader.hpp",
        "ffx_upscale_backend_dx12.hpp",
        "ffx_support_probe_dx12.hpp",
    ]
    for filename in sources:
        source = bridge_root / "src" / filename
        destination = dlss_dir / filename
        if not source.is_file():
            raise FileNotFoundError(source)
        shutil.copy2(source, destination)

    patch_amd_bridge(dlss_dir / "amd_bridge.hpp")
    patch_ffx_backend(dlss_dir / "ffx_upscale_backend_dx12.hpp")
    patch_streamline(dlss_dir / "streamline_v2.hpp")

    print("RenoDX AMD bridge materialized successfully")


if __name__ == "__main__":
    main()
