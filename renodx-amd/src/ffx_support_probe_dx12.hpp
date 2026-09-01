/*
 * Early AMD FidelityFX DX12 support probe for Streamline feature queries.
 *
 * This is intentionally independent of the live backend/runtime state. It uses
 * Streamline's DXGI adapter LUID to create a short-lived D3D12 device, asks the
 * signed FidelityFX loader whether an upscaler provider exists for that device,
 * then releases everything before returning.
 */

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <sl.h>

#if __has_include(<ffx_api.h>) && __has_include(<ffx_upscale.h>)
#include <ffx_api.h>
#include <ffx_upscale.h>
#define RENODX_AMD_HAS_FFX_SUPPORT_PROBE 1
#elif __has_include("ffx_api.h") && __has_include("ffx_upscale.h")
#include "ffx_api.h"
#include "ffx_upscale.h"
#define RENODX_AMD_HAS_FFX_SUPPORT_PROBE 1
#else
#define RENODX_AMD_HAS_FFX_SUPPORT_PROBE 0
#endif

namespace renodx::utils::dlss::amd_bridge::ffx_dx12 {

#if RENODX_AMD_HAS_FFX_SUPPORT_PROBE
namespace support_probe {

constexpr uint32_t kAmdVendorId = 0x1002u;
inline std::mutex mutex;
inline std::unordered_map<uint64_t, bool> cache;
inline std::atomic_bool any_positive_probe = false;

inline uint64_t LuidKey(const LUID& luid) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32u)
      | static_cast<uint32_t>(luid.LowPart);
}

inline HMODULE LoadFfxLoader(const std::filesystem::path& directory) {
  constexpr std::array<std::wstring_view, 2> candidates = {
      L"amd_fidelityfx_loader_dx12.dll",
      L"amd_fidelityfx_dx12.dll",
  };

  for (const auto filename : candidates) {
    if (auto module = LoadLibraryW((directory / filename).c_str()); module != nullptr) {
      return module;
    }
  }
  return nullptr;
}

inline bool ProbeUncached(const LUID& luid, const std::filesystem::path& runtime_directory) {
  IDXGIFactory4* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || factory == nullptr) return false;

  IDXGIAdapter1* adapter = nullptr;
  const auto adapter_result = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
  factory->Release();
  if (FAILED(adapter_result) || adapter == nullptr) return false;

  DXGI_ADAPTER_DESC1 desc{};
  const auto desc_result = adapter->GetDesc1(&desc);
  if (FAILED(desc_result) || desc.VendorId != kAmdVendorId) {
    adapter->Release();
    return false;
  }

  ID3D12Device* probe_device = nullptr;
  const auto device_result = D3D12CreateDevice(
      adapter,
      D3D_FEATURE_LEVEL_11_0,
      IID_PPV_ARGS(&probe_device));
  adapter->Release();
  if (FAILED(device_result) || probe_device == nullptr) return false;

  HMODULE module = LoadFfxLoader(runtime_directory);
  if (module == nullptr) {
    probe_device->Release();
    return false;
  }

  const auto query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(module, "ffxQuery"));
  bool supported = false;
  if (query != nullptr) {
    uint64_t version_count = 0u;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = probe_device;
    versions.outputCount = &version_count;

    supported = query(nullptr, &versions.header) == FFX_API_RETURN_OK
        && version_count != 0u;
  }

  FreeLibrary(module);
  probe_device->Release();
  return supported;
}

}  // namespace support_probe

inline bool ProbeAdapterSupport(
    const sl::AdapterInfo& adapter_info,
    const std::filesystem::path& runtime_directory = L".") {
  if (adapter_info.vkPhysicalDevice != nullptr
      || adapter_info.deviceLUID == nullptr
      || adapter_info.deviceLUIDSizeInBytes != sizeof(LUID)) {
    return false;
  }

  LUID luid{};
  std::memcpy(&luid, adapter_info.deviceLUID, sizeof(luid));
  const auto key = support_probe::LuidKey(luid);

  std::scoped_lock lock(support_probe::mutex);
  if (const auto it = support_probe::cache.find(key); it != support_probe::cache.end()) {
    return it->second;
  }

  const bool supported = support_probe::ProbeUncached(luid, runtime_directory);
  support_probe::cache.emplace(key, supported);
  if (supported) support_probe::any_positive_probe.store(true, std::memory_order_release);
  return supported;
}

[[nodiscard]] inline bool HasPositiveAdapterProbe() {
  return support_probe::any_positive_probe.load(std::memory_order_acquire);
}

#else

inline bool ProbeAdapterSupport(const sl::AdapterInfo&, const std::filesystem::path& = L".") { return false; }
[[nodiscard]] inline bool HasPositiveAdapterProbe() { return false; }

#endif

}  // namespace renodx::utils::dlss::amd_bridge::ffx_dx12
