/*
 * Experimental runtime loader for AMD FidelityFX API DX12 binaries.
 *
 * When the official FidelityFX SDK headers are on the include path this loader
 * exposes strongly typed ffxCreateContext/ffxDestroyContext/ffxDispatch/
 * ffxQuery/ffxConfigure entry points. The bridge itself does not vendor AMD's
 * ABI declarations.
 */

#pragma once

#include <array>
#include <filesystem>
#include <string_view>
#include <type_traits>

#include <Windows.h>

#if __has_include(<ffx_api.h>)
#include <ffx_api.h>
#define RENODX_AMD_HAS_FFX_API 1
#elif __has_include("ffx_api.h")
#include "ffx_api.h"
#define RENODX_AMD_HAS_FFX_API 1
#else
#define RENODX_AMD_HAS_FFX_API 0
#endif

namespace renodx::utils::dlss::amd_bridge::ffx {

#if RENODX_AMD_HAS_FFX_API
using CreateContextFn = PfnFfxCreateContext;
using DestroyContextFn = PfnFfxDestroyContext;
using DispatchFn = PfnFfxDispatch;
using QueryFn = PfnFfxQuery;
using ConfigureFn = PfnFfxConfigure;
#else
using CreateContextFn = FARPROC;
using DestroyContextFn = FARPROC;
using DispatchFn = FARPROC;
using QueryFn = FARPROC;
using ConfigureFn = FARPROC;
#endif

struct Runtime {
  HMODULE module = nullptr;
  CreateContextFn create_context = nullptr;
  DestroyContextFn destroy_context = nullptr;
  DispatchFn dispatch = nullptr;
  QueryFn query = nullptr;
  ConfigureFn configure = nullptr;

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

template <typename T>
inline T LoadProc(HMODULE module, const char* name) {
  static_assert(std::is_pointer_v<T>);
  return reinterpret_cast<T>(GetProcAddress(module, name));
}
}  // namespace internal

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
  candidate.create_context = internal::LoadProc<CreateContextFn>(module, "ffxCreateContext");
  candidate.destroy_context = internal::LoadProc<DestroyContextFn>(module, "ffxDestroyContext");
  candidate.dispatch = internal::LoadProc<DispatchFn>(module, "ffxDispatch");
  candidate.query = internal::LoadProc<QueryFn>(module, "ffxQuery");
  candidate.configure = internal::LoadProc<ConfigureFn>(module, "ffxConfigure");

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

  // Prefer the current FidelityFX API loader. The legacy combined DX12 name
  // remains a fallback for older SDK/runtime layouts used by existing mods.
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
