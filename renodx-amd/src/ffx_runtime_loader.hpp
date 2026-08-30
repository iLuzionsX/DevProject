/*
 * Experimental runtime loader for AMD FSR API DX12 binaries.
 *
 * This intentionally does not reproduce AMD SDK ABI structs. The dispatch
 * implementation should include the official ffx_api headers and cast these
 * exports to the SDK function-pointer types there.
 */

#pragma once

#include <array>
#include <filesystem>
#include <string_view>

#include <Windows.h>

namespace renodx::utils::dlss::amd_bridge::ffx {

struct Runtime {
  HMODULE module = nullptr;
  FARPROC create_context = nullptr;
  FARPROC destroy_context = nullptr;
  FARPROC dispatch = nullptr;
  FARPROC query = nullptr;
  FARPROC configure = nullptr;

  [[nodiscard]] bool IsReady() const {
    return module != nullptr
        && create_context != nullptr
        && destroy_context != nullptr
        && dispatch != nullptr
        && query != nullptr
        && configure != nullptr;
  }
};

namespace internal {
inline Runtime runtime;
}

inline void Unload() {
  if (internal::runtime.module != nullptr) {
    FreeLibrary(internal::runtime.module);
  }
  internal::runtime = {};
}

inline bool TryLoadModule(const std::filesystem::path& path) {
  auto module = LoadLibraryW(path.c_str());
  if (module == nullptr) return false;

  Runtime candidate;
  candidate.module = module;
  candidate.create_context = GetProcAddress(module, "ffxCreateContext");
  candidate.destroy_context = GetProcAddress(module, "ffxDestroyContext");
  candidate.dispatch = GetProcAddress(module, "ffxDispatch");
  candidate.query = GetProcAddress(module, "ffxQuery");
  candidate.configure = GetProcAddress(module, "ffxConfigure");

  if (!candidate.IsReady()) {
    FreeLibrary(module);
    return false;
  }

  Unload();
  internal::runtime = candidate;
  return true;
}

inline bool Load(const std::filesystem::path& directory = L".") {
  if (internal::runtime.IsReady()) return true;

  // FSR SDK 2.x split the loader and upscaler DLLs. The loader remains ABI
  // compatible with the previous combined DX12 DLL, so prefer it first.
  constexpr std::array<std::wstring_view, 2> candidates = {
      L"amd_fidelityfx_loader_dx12.dll",
      L"amd_fidelityfx_dx12.dll",
  };

  for (const auto filename : candidates) {
    if (TryLoadModule(directory / filename)) return true;
  }

  return false;
}

[[nodiscard]] inline const Runtime& GetRuntime() {
  return internal::runtime;
}

}  // namespace renodx::utils::dlss::amd_bridge::ffx
