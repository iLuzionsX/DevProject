#!/usr/bin/env python3
"""Apply Streamline contract hardening after materialize_integration.py.

This layer intentionally stays separate from build recovery so it can be
reviewed/tested as the next stack: render-API gating and consistent DLSS feature
requirements for the DX12-only FidelityFX backend.
"""

from __future__ import annotations

import argparse
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one anchor, found {count}")
    return text.replace(old, new, 1)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--renodx-root", required=True, type=Path)
    args = parser.parse_args()

    path = args.renodx_root.resolve() / "src" / "utils" / "dlss" / "streamline_v2.hpp"
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        "static decltype(&slIsFeatureLoaded) Real_slIsFeatureLoaded = nullptr;\n"
        "static decltype(&slSetConstants) Real_slSetConstants = nullptr;",
        "static decltype(&slIsFeatureLoaded) Real_slIsFeatureLoaded = nullptr;\n"
        "static decltype(&slGetFeatureRequirements) Real_slGetFeatureRequirements = nullptr;\n"
        "static decltype(&slSetConstants) Real_slSetConstants = nullptr;",
        "declare feature requirements function",
    )

    text = replace_once(
        text,
        "static void ProcessDeferredStreamlineUpgrades();\n\n"
        "SL_API sl::Result Hooked_slInit(const sl::Preferences& pref, uint64_t sdkVersion = sl::kSDKVersion) {",
        "static void ProcessDeferredStreamlineUpgrades();\n\n"
        "// The alternate FidelityFX backend is DX12-only. Keep the Streamline\n"
        "// session API explicit so pre-device support queries cannot accidentally\n"
        "// advertise AMD support for a D3D11 or Vulkan integration.\n"
        "static sl::RenderAPI amd_bridge_render_api = sl::RenderAPI::eCount;\n\n"
        "SL_API sl::Result Hooked_slInit(const sl::Preferences& pref, uint64_t sdkVersion = sl::kSDKVersion) {\n"
        "  amd_bridge_render_api = pref.renderAPI;",
        "capture Streamline render API",
    )

    text = replace_once(
        text,
        "SL_API sl::Result Hooked_slShutdown() {\n"
        "  renodx::utils::dlss::amd_bridge::ffx_dx12::Shutdown();\n"
        "  return Real_slShutdown();\n"
        "}",
        "SL_API sl::Result Hooked_slShutdown() {\n"
        "  renodx::utils::dlss::amd_bridge::ffx_dx12::Shutdown();\n"
        "  amd_bridge_render_api = sl::RenderAPI::eCount;\n"
        "  return Real_slShutdown();\n"
        "}",
        "reset Streamline render API",
    )

    text = replace_once(
        text,
        "  if (feature == sl::kFeatureDLSS) {\n"
        "    if (renodx::utils::dlss::amd_bridge::IsBackendAvailable()\n"
        "        || renodx::utils::dlss::amd_bridge::ffx_dx12::ProbeAdapterSupport(adapterInfo)) {\n"
        "      return sl::Result::eOk;\n"
        "    }\n"
        "  }",
        "  if (feature == sl::kFeatureDLSS\n"
        "      && amd_bridge_render_api == sl::RenderAPI::eD3D12) {\n"
        "    if (renodx::utils::dlss::amd_bridge::IsBackendAvailable()\n"
        "        || renodx::utils::dlss::amd_bridge::ffx_dx12::ProbeAdapterSupport(adapterInfo)) {\n"
        "      return sl::Result::eOk;\n"
        "    }\n"
        "  }",
        "gate feature support by render API",
    )

    loaded_block = (
        "static sl::Result Hooked_slIsFeatureLoaded(sl::Feature feature, bool& loaded) {\n"
        "  if (feature == sl::kFeatureDLSS\n"
        "      && renodx::utils::dlss::amd_bridge::IsBackendAvailable()) {\n"
        "    loaded = true;\n"
        "    return sl::Result::eOk;\n"
        "  }\n"
        "  return Real_slIsFeatureLoaded(feature, loaded);\n"
        "}\n"
    )
    requirements_block = (
        loaded_block
        + "\nstatic constexpr sl::BufferType kAmdDlssRequiredTags[] = {\n"
        "    sl::kBufferTypeDepth,\n"
        "    sl::kBufferTypeMotionVectors,\n"
        "    sl::kBufferTypeScalingInputColor,\n"
        "    sl::kBufferTypeScalingOutputColor,\n"
        "};\n\n"
        "static sl::Result Hooked_slGetFeatureRequirements(\n"
        "    sl::Feature feature, sl::FeatureRequirements& requirements) {\n"
        "  if (feature != sl::kFeatureDLSS\n"
        "      || amd_bridge_render_api != sl::RenderAPI::eD3D12) {\n"
        "    return Real_slGetFeatureRequirements(feature, requirements);\n"
        "  }\n\n"
        "  // Requirements are a session/API contract, not an adapter support\n"
        "  // decision. Adapter/provider eligibility remains in slIsFeatureSupported.\n"
        "  const auto real_result = Real_slGetFeatureRequirements(feature, requirements);\n"
        "  if (real_result != sl::Result::eOk) {\n"
        "    requirements = sl::FeatureRequirements{};\n"
        "  }\n"
        "  requirements.flags = sl::FeatureRequirementFlags::eD3D12Supported;\n"
        "  requirements.maxNumCPUThreads = 0u;\n"
        "  requirements.maxNumViewports = std::numeric_limits<uint32_t>::max();\n"
        "  requirements.numRequiredTags = static_cast<uint32_t>(std::size(kAmdDlssRequiredTags));\n"
        "  requirements.requiredTags = kAmdDlssRequiredTags;\n"
        "  return sl::Result::eOk;\n"
        "}\n"
    )
    text = replace_once(text, loaded_block, requirements_block, "virtualize DLSS requirements")

    text = replace_once(
        text,
        "  if (d3d12_device != nullptr) {\n"
        "    renodx::utils::dlss::amd_bridge::ffx_dx12::Initialize(d3d12_device);\n"
        "    d3d12_device->Release();\n"
        "  }",
        "  if (d3d12_device != nullptr) {\n"
        "    if (amd_bridge_render_api == sl::RenderAPI::eD3D12) {\n"
        "      renodx::utils::dlss::amd_bridge::ffx_dx12::Initialize(d3d12_device);\n"
        "    }\n"
        "    d3d12_device->Release();\n"
        "  }",
        "gate backend initialization by render API",
    )

    text = replace_once(
        text,
        "    {\"slIsFeatureLoaded\", reinterpret_cast<void**>(&Real_slIsFeatureLoaded), &Hooked_slIsFeatureLoaded},\n"
        "    {\"slSetConstants\", reinterpret_cast<void**>(&Real_slSetConstants), &Hooked_slSetConstants},",
        "    {\"slIsFeatureLoaded\", reinterpret_cast<void**>(&Real_slIsFeatureLoaded), &Hooked_slIsFeatureLoaded},\n"
        "    {\"slGetFeatureRequirements\", reinterpret_cast<void**>(&Real_slGetFeatureRequirements), &Hooked_slGetFeatureRequirements},\n"
        "    {\"slSetConstants\", reinterpret_cast<void**>(&Real_slSetConstants), &Hooked_slSetConstants},",
        "hook feature requirements",
    )

    path.write_text(text, encoding="utf-8", newline="\n")
    print("Streamline AMD contract hardening applied")


if __name__ == "__main__":
    main()
