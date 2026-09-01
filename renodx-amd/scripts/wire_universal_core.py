#!/usr/bin/env python3
"""Wire the provider-neutral rendering core into the materialized RenoDX bridge.

This layer remains behavior-preserving for the current milestone: the live
FidelityFX DX12 backend still dispatches only with native Streamline motion
vectors. Frames without native motion are now allowed to reach UniversalFrame /
PlanRoute, where reconstructed routes remain disabled until a real GPU motion
resource can be materialized.
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
        "    // Motion may be reconstructed by the universal layer. Keep the outer\n"
        "    // bridge gate limited to inputs every temporal route needs, then let\n"
        "    // backend preflight decide whether native/reconstructed motion is usable.\n"
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
        '#include "universal/routing.hpp"\n'
        '#include "universal/streamline_frame_adapter.hpp"',
        "universal backend includes",
    )

    preflight_anchor = "inline bool BridgePreflight(\n"
    universal_capabilities = (
        "inline renodx::universal::BackendCapabilities UniversalCapabilities() {\n"
        "  renodx::universal::BackendCapabilities capabilities{};\n"
        "  capabilities.graphics_api_mask =\n"
        "      renodx::universal::GraphicsApiBit(renodx::universal::GraphicsApi::kD3D12);\n"
        "  capabilities.requires_depth = true;\n"
        "  capabilities.requires_motion_vectors = true;\n"
        "  // Reconstructed motion becomes eligible only after the reconstruction\n"
        "  // pipeline can materialize a real GPU motion-vector resource.\n"
        "  capabilities.accepts_reconstructed_motion = false;\n"
        "  capabilities.supports_auto_exposure = true;\n"
        "  capabilities.supports_reactive_mask = true;\n"
        "  capabilities.supports_transparency_mask = true;\n"
        "  capabilities.supports_hdr = true;\n"
        "  return capabilities;\n"
        "}\n\n"
    )
    text = replace_once(
        text,
        preflight_anchor,
        universal_capabilities + preflight_anchor,
        "universal capabilities insertion",
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
        "  if (!initialized || device == nullptr || !ffx::GetRuntime().IsReady()) return false;\n\n"
        "  const auto render_size = ResourceDimensions(state.resources.scaling_input);\n"
        "  const auto output_size = ResourceDimensions(state.resources.scaling_output);\n"
        "  const auto depth_size = ResourceDimensions(state.resources.depth);\n"
        "  const auto motion_size = ResourceDimensions(state.resources.motion_vectors);\n\n"
        "  renodx::universal::streamline::FrameExtents universal_extents{};\n"
        "  universal_extents.render = {render_size.width, render_size.height};\n"
        "  universal_extents.output = {output_size.width, output_size.height};\n"
        "  universal_extents.depth = {depth_size.width, depth_size.height};\n"
        "  universal_extents.motion = {motion_size.width, motion_size.height};\n"
        "  const auto universal_frame =\n"
        "      renodx::universal::streamline::ToUniversalFrame(state, universal_extents);\n"
        "  const auto universal_route = renodx::universal::PlanRoute(\n"
        "      universal_frame, UniversalCapabilities());\n"
        "  if (!universal_route.eligible\n"
        "      || universal_route.tier != renodx::universal::InputTier::kNativeTemporal) {\n"
        "    return false;\n"
        "  }\n\n"
        "  // Native-only backend checks intentionally happen after neutral routing.\n"
        "  // This keeps the missing-MV path reachable without changing dispatch yet.\n"
        "  if (!RequiredResourcesReady(state)) return false;\n\n"
        "  const auto flags = BuildCreateFlags(state);\n"
        "  return CreateContextLocked(state.viewport, render_size, output_size, flags);"
    )
    text = replace_once(
        text,
        old_preflight,
        new_preflight,
        "universal preflight routing",
    )

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
