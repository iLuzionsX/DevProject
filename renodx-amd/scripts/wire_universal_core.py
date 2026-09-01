#!/usr/bin/env python3
"""Wire the provider-neutral rendering core into the materialized RenoDX bridge.

Native motion vectors remain the preferred path. When they are absent, the
neutral router may select camera reconstruction only if the DX12 reconstruction
executor has successfully loaded its shader and can allocate a compatible
motion target during preflight.
"""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one anchor, found {count}")
    return text.replace(old, new, 1)


def copy_universal_headers(bridge_root: Path, renodx_root: Path) -> None:
    source = bridge_root / "src" / "universal"
    destination = renodx_root / "src" / "utils" / "dlss" / "universal"
    if not source.is_dir():
        raise FileNotFoundError(source)

    destination.mkdir(parents=True, exist_ok=True)
    for header in sorted(source.glob("*.hpp")):
        shutil.copy2(header, destination / header.name)


def patch_bridge_gate(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    old_gate = (
        "  [[nodiscard]] bool IsReadyForUpscale() const {\n"
        "    return resources.HasMinimumUpscaleInputs() && constants.has_value();\n"
        "  }"
    )
    new_gate = (
        "  [[nodiscard]] bool IsReadyForUpscale() const {\n"
        "    // Motion may be reconstructed. Preflight is responsible for proving\n"
        "    // that either native vectors or a safe reconstruction path is ready.\n"
        "    return resources.depth.IsValid()\n"
        "        && resources.scaling_input.IsValid()\n"
        "        && resources.scaling_output.IsValid()\n"
        "        && constants.has_value();\n"
        "  }"
    )
    text = replace_once(text, old_gate, new_gate, "universal outer bridge gate")
    path.write_text(text, encoding="utf-8", newline="\n")


def patch_ffx_backend(path: Path) -> None:
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        '#include "amd_bridge.hpp"\n#include "ffx_runtime_loader.hpp"',
        '#include "amd_bridge.hpp"\n'
        '#include "ffx_runtime_loader.hpp"\n'
        '#include "universal/backend.hpp"\n'
        '#include "universal/camera_motion_dx12.hpp"\n'
        '#include "universal/routing.hpp"\n'
        '#include "universal/streamline_frame_adapter.hpp"',
        "universal backend includes",
    )

    needs_transition = "inline bool NeedsTransition(D3D12_RESOURCE_STATES current, D3D12_RESOURCE_STATES required) {\n"
    common_ready = (
        "inline bool RequiredCommonResourcesReady(const FrameState& state) {\n"
        "  const auto& resources = state.resources;\n"
        "  if (!IsKnownState(resources.scaling_input)\n"
        "      || !IsKnownState(resources.scaling_output)\n"
        "      || !IsKnownState(resources.depth)) {\n"
        "    return false;\n"
        "  }\n"
        "  if (HasUnsupportedOffset(resources.scaling_input)\n"
        "      || HasUnsupportedOffset(resources.scaling_output)\n"
        "      || HasUnsupportedOffset(resources.depth)) {\n"
        "    return false;\n"
        "  }\n"
        "  const auto render_size = ResourceDimensions(resources.scaling_input);\n"
        "  const auto output_size = ResourceDimensions(resources.scaling_output);\n"
        "  if (!render_size.IsValid() || !output_size.IsValid()) return false;\n"
        "  auto* output = NativeResource(resources.scaling_output);\n"
        "  if (output == nullptr\n"
        "      || (output->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {\n"
        "    return false;\n"
        "  }\n"
        "  if (!state.constants.has_value()) return false;\n"
        "  const auto& constants = *state.constants;\n"
        "  if (!IsValidSlFloat(constants.jitterOffset.x)\n"
        "      || !IsValidSlFloat(constants.jitterOffset.y)\n"
        "      || !IsValidSlFloat(constants.cameraNear)\n"
        "      || !IsValidSlFloat(constants.cameraFar)\n"
        "      || !IsValidSlFloat(constants.cameraFOV)\n"
        "      || constants.cameraFOV <= 0.0f) {\n"
        "    return false;\n"
        "  }\n"
        "  const auto* options = DlssOptions(state);\n"
        "  if (options != nullptr && options->mode == sl::DLSSMode::eOff) return false;\n"
        "  if (!IsUsableOptionalResource(resources.exposure) && !UseAutoExposure(state)) return false;\n"
        "  return true;\n"
        "}\n\n"
    )
    text = replace_once(
        text,
        needs_transition,
        common_ready + needs_transition,
        "universal common resource gate",
    )

    preflight_anchor = "inline bool BridgePreflight(\n"
    universal_helpers = (
        "inline renodx::universal::BackendCapabilities UniversalCapabilities() {\n"
        "  renodx::universal::BackendCapabilities capabilities{};\n"
        "  capabilities.graphics_api_mask =\n"
        "      renodx::universal::GraphicsApiBit(renodx::universal::GraphicsApi::kD3D12);\n"
        "  capabilities.requires_depth = true;\n"
        "  capabilities.requires_motion_vectors = true;\n"
        "  capabilities.accepts_reconstructed_motion =\n"
        "      renodx::universal::camera_motion_dx12::IsReady();\n"
        "  capabilities.supports_auto_exposure = true;\n"
        "  capabilities.supports_reactive_mask = true;\n"
        "  capabilities.supports_transparency_mask = true;\n"
        "  capabilities.supports_hdr = true;\n"
        "  return capabilities;\n"
        "}\n\n"
        "inline renodx::universal::UniversalFrame BuildUniversalFrame(const FrameState& state) {\n"
        "  const auto render_size = ResourceDimensions(state.resources.scaling_input);\n"
        "  const auto output_size = ResourceDimensions(state.resources.scaling_output);\n"
        "  const auto depth_size = ResourceDimensions(state.resources.depth);\n"
        "  const auto motion_size = ResourceDimensions(state.resources.motion_vectors);\n"
        "  renodx::universal::streamline::FrameExtents extents{};\n"
        "  extents.render = {render_size.width, render_size.height};\n"
        "  extents.output = {output_size.width, output_size.height};\n"
        "  extents.depth = {depth_size.width, depth_size.height};\n"
        "  extents.motion = {motion_size.width, motion_size.height};\n"
        "  return renodx::universal::streamline::ToUniversalFrame(state, extents);\n"
        "}\n\n"
        "inline uint32_t BuildCreateFlagsForRoute(\n"
        "    const FrameState& state, bool reconstructed_motion) {\n"
        "  auto flags = BuildCreateFlags(state);\n"
        "  if (reconstructed_motion) {\n"
        "    flags &= ~FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;\n"
        "    flags &= ~FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;\n"
        "  }\n"
        "  return flags;\n"
        "}\n\n"
    )
    text = replace_once(
        text,
        preflight_anchor,
        universal_helpers + preflight_anchor,
        "universal helpers insertion",
    )

    old_preflight = (
        "  if (!initialized || device == nullptr || !ffx::GetRuntime().IsReady()) return false;\n"
        "  if (!RequiredResourcesReady(state)) return false;\n\n"
        "  const auto render_size = ResourceDimensions(state.resources.scaling_input);\n"
        "  const auto output_size = ResourceDimensions(state.resources.scaling_output);\n"
        "  const auto flags = BuildCreateFlags(state);\n"
        "  return CreateContextLocked(state.viewport, render_size, output_size, flags);"
    )
    new_preflight = (
        "  if (!initialized || device == nullptr || !ffx::GetRuntime().IsReady()) return false;\n"
        "  if (!RequiredCommonResourcesReady(state)) return false;\n\n"
        "  const auto universal_frame = BuildUniversalFrame(state);\n"
        "  const auto route = renodx::universal::PlanRoute(\n"
        "      universal_frame, UniversalCapabilities());\n"
        "  if (!route.eligible) return false;\n\n"
        "  const bool reconstructed_motion =\n"
        "      route.motion.source == renodx::universal::MotionSource::kCameraReprojection;\n"
        "  if (route.motion.source == renodx::universal::MotionSource::kNative) {\n"
        "    if (!RequiredResourcesReady(state)) return false;\n"
        "  } else if (reconstructed_motion) {\n"
        "    if (!renodx::universal::camera_motion_dx12::Prepare(\n"
        "            state.viewport, universal_frame, NativeResource(state.resources.depth))) {\n"
        "      return false;\n"
        "    }\n"
        "  } else {\n"
        "    return false;\n"
        "  }\n\n"
        "  const auto render_size = ResourceDimensions(state.resources.scaling_input);\n"
        "  const auto output_size = ResourceDimensions(state.resources.scaling_output);\n"
        "  const auto flags = BuildCreateFlagsForRoute(state, reconstructed_motion);\n"
        "  return CreateContextLocked(state.viewport, render_size, output_size, flags);"
    )
    text = replace_once(text, old_preflight, new_preflight, "universal preflight routing")

    evaluate_runtime_anchor = (
        "  const auto& runtime = ffx::GetRuntime();\n"
        "  if (!runtime.IsReady()) return sl::Result::eErrorNotInitialized;\n\n"
        "  const auto& resources = state.resources;"
    )
    evaluate_runtime_new = (
        "  const auto& runtime = ffx::GetRuntime();\n"
        "  if (!runtime.IsReady()) return sl::Result::eErrorNotInitialized;\n\n"
        "  const auto universal_frame = BuildUniversalFrame(state);\n"
        "  const auto route = renodx::universal::PlanRoute(\n"
        "      universal_frame, UniversalCapabilities());\n"
        "  if (!route.eligible) return sl::Result::eErrorMissingInputParameter;\n"
        "  const bool reconstructed_motion =\n"
        "      route.motion.source == renodx::universal::MotionSource::kCameraReprojection;\n"
        "  if (route.motion.source == renodx::universal::MotionSource::kNative) {\n"
        "    if (!RequiredResourcesReady(state)) return sl::Result::eErrorMissingInputParameter;\n"
        "  } else if (reconstructed_motion) {\n"
        "    if (!RequiredCommonResourcesReady(state)\n"
        "        || !renodx::universal::camera_motion_dx12::Prepare(\n"
        "            state.viewport, universal_frame, NativeResource(state.resources.depth))) {\n"
        "      return sl::Result::eErrorMissingInputParameter;\n"
        "    }\n"
        "  } else {\n"
        "    return sl::Result::eErrorFeatureNotSupported;\n"
        "  }\n\n"
        "  const auto& resources = state.resources;"
    )
    text = replace_once(
        text,
        evaluate_runtime_anchor,
        evaluate_runtime_new,
        "universal evaluate routing",
    )

    motion_dimensions_old = (
        "  const auto motion_vector_size = ResourceDimensions(resources.motion_vectors);\n\n"
        "  if (!motion_vector_size.IsValid()) {\n"
        "    return sl::Result::eErrorMissingInputParameter;\n"
        "  }"
    )
    motion_dimensions_new = (
        "  const auto motion_vector_size = reconstructed_motion\n"
        "      ? render_size\n"
        "      : ResourceDimensions(resources.motion_vectors);\n\n"
        "  if (!motion_vector_size.IsValid()) {\n"
        "    return sl::Result::eErrorMissingInputParameter;\n"
        "  }"
    )
    text = replace_once(
        text,
        motion_dimensions_old,
        motion_dimensions_new,
        "reconstructed motion dimensions",
    )

    text = replace_once(
        text,
        "  TransitionResource(command_list, resources.motion_vectors, kReadState, restore);",
        "  if (!reconstructed_motion) {\n"
        "    TransitionResource(command_list, resources.motion_vectors, kReadState, restore);\n"
        "  }",
        "conditional native motion transition",
    )

    dispatch_anchor = "  ffxDispatchDescUpscale dispatch{};\n"
    dispatch_preamble = (
        "  ID3D12Resource* motion_resource = NativeResource(resources.motion_vectors);\n"
        "  if (reconstructed_motion) {\n"
        "    motion_resource = renodx::universal::camera_motion_dx12::Dispatch(\n"
        "        command_list,\n"
        "        state.viewport,\n"
        "        universal_frame,\n"
        "        NativeResource(resources.depth));\n"
        "    if (motion_resource == nullptr) {\n"
        "      RestoreResourceStates(command_list, restore);\n"
        "      return sl::Result::eErrorComputeFailed;\n"
        "    }\n"
        "  }\n\n"
    )
    text = replace_once(
        text,
        dispatch_anchor,
        dispatch_preamble + dispatch_anchor,
        "camera motion dispatch",
    )

    text = replace_once(
        text,
        "  dispatch.motionVectors = ReadResource(resources.motion_vectors);",
        "  dispatch.motionVectors = ffxApiGetResourceDX12(\n"
        "      motion_resource, FFX_API_RESOURCE_STATE_COMPUTE_READ);",
        "reconstructed FFX motion resource",
    )

    native_scale = (
        "  dispatch.motionVectorScale = {\n"
        "      constants.mvecScale.x * static_cast<float>(motion_vector_size.width),\n"
        "      constants.mvecScale.y * static_cast<float>(motion_vector_size.height)};"
    )
    routed_scale = (
        "  dispatch.motionVectorScale = reconstructed_motion\n"
        "      ? FfxApiFloatCoords2D{1.0f, 1.0f}\n"
        "      : FfxApiFloatCoords2D{\n"
        "          constants.mvecScale.x * static_cast<float>(motion_vector_size.width),\n"
        "          constants.mvecScale.y * static_cast<float>(motion_vector_size.height)};"
    )
    text = replace_once(text, native_scale, routed_scale, "reconstructed FFX motion scale")

    initialize_anchor = (
        "  if (device == nullptr || !internal::IsAmdDevice(device) || !ffx::Load(runtime_directory)) return false;\n\n"
        "  std::scoped_lock lock(internal::backend_mutex);"
    )
    initialize_new = (
        "  if (device == nullptr || !internal::IsAmdDevice(device) || !ffx::Load(runtime_directory)) return false;\n\n"
        "  // Failure here is non-fatal: native game motion can still use FidelityFX.\n"
        "  renodx::universal::camera_motion_dx12::Initialize(\n"
        "      device, runtime_directory / L\"camera_motion_cs.dxil\");\n\n"
        "  std::scoped_lock lock(internal::backend_mutex);"
    )
    text = replace_once(text, initialize_anchor, initialize_new, "camera motion initialization")

    init_failure_old = (
        "    internal::device->Release();\n"
        "    internal::device = nullptr;\n"
        "    ffx::Unload();\n"
        "    return false;"
    )
    init_failure_new = (
        "    internal::device->Release();\n"
        "    internal::device = nullptr;\n"
        "    renodx::universal::camera_motion_dx12::Shutdown();\n"
        "    ffx::Unload();\n"
        "    return false;"
    )
    text = replace_once(text, init_failure_old, init_failure_new, "camera motion init rollback")

    shutdown_old = (
        "  if (internal::device != nullptr) {\n"
        "    internal::device->Release();\n"
        "    internal::device = nullptr;\n"
        "  }\n"
        "  ffx::Unload();"
    )
    shutdown_new = (
        "  if (internal::device != nullptr) {\n"
        "    internal::device->Release();\n"
        "    internal::device = nullptr;\n"
        "  }\n"
        "  renodx::universal::camera_motion_dx12::Shutdown();\n"
        "  ffx::Unload();"
    )
    text = replace_once(text, shutdown_old, shutdown_new, "camera motion shutdown")

    path.write_text(text, encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bridge-root", required=True, type=Path)
    parser.add_argument("--renodx-root", required=True, type=Path)
    args = parser.parse_args()

    bridge_root = args.bridge_root.resolve()
    renodx_root = args.renodx_root.resolve()

    copy_universal_headers(bridge_root, renodx_root)
    patch_bridge_gate(renodx_root / "src" / "utils" / "dlss" / "amd_bridge.hpp")
    patch_ffx_backend(
        renodx_root / "src" / "utils" / "dlss" / "ffx_upscale_backend_dx12.hpp"
    )
    print("Universal rendering core wired into RenoDX AMD bridge")


if __name__ == "__main__":
    main()
