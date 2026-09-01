#pragma once

#include <cstdint>
#include <string_view>

#include "universal_frame.hpp"

namespace renodx::universal {

enum class BackendKind : uint8_t {
  kUnknown = 0,
  kFidelityFx,
  kXeSs,
  kNvidiaDlss,
};

enum class BackendResult : uint8_t {
  // Backend declined before recording GPU work. The router may try another
  // provider or the game's native path.
  kDeclined = 0,
  // Backend owns the evaluation and completed it successfully.
  kOwnedSuccess,
  // Backend owns the evaluation but dispatch failed. No second backend may be
  // invoked on the same command context/frame after this result.
  kOwnedFailure,
};

[[nodiscard]] constexpr uint32_t GraphicsApiBit(GraphicsApi api) noexcept {
  switch (api) {
    case GraphicsApi::kD3D11:
      return 1u << 0u;
    case GraphicsApi::kD3D12:
      return 1u << 1u;
    case GraphicsApi::kVulkan:
      return 1u << 2u;
    default:
      return 0u;
  }
}

struct BackendCapabilities {
  uint32_t graphics_api_mask = 0u;
  bool requires_depth = true;
  bool requires_motion_vectors = true;
  bool accepts_reconstructed_motion = true;
  bool supports_auto_exposure = false;
  bool supports_reactive_mask = false;
  bool supports_transparency_mask = false;
  bool supports_hdr = true;

  [[nodiscard]] constexpr bool SupportsApi(GraphicsApi api) const noexcept {
    return (graphics_api_mask & GraphicsApiBit(api)) != 0u;
  }
};

struct BackendDevice {
  GraphicsApi api = GraphicsApi::kUnknown;
  void* native_device = nullptr;
  void* native_queue = nullptr;
  uint32_t vendor_id = 0u;
  uint32_t device_id = 0u;
};

struct BackendCreateDesc {
  BackendDevice device{};
  Extent2D max_render_extent{};
  Extent2D max_output_extent{};
};

struct DispatchContext {
  // ID3D11DeviceContext*, ID3D12GraphicsCommandList*, VkCommandBuffer, or an
  // interop-owned command context. Interpretation belongs to the backend.
  void* native_commands = nullptr;
  GraphicsApi api = GraphicsApi::kUnknown;
};

class IRenderBackend {
 public:
  virtual ~IRenderBackend() = default;

  [[nodiscard]] virtual std::string_view Name() const noexcept = 0;
  [[nodiscard]] virtual BackendKind Kind() const noexcept = 0;
  [[nodiscard]] virtual BackendCapabilities Capabilities() const noexcept = 0;

  // Must not record GPU work. Returning false preserves the caller's ability
  // to fall through to another provider/native implementation.
  [[nodiscard]] virtual bool Preflight(const UniversalFrame& frame) const noexcept = 0;

  virtual bool Initialize(const BackendCreateDesc& desc) = 0;
  virtual void Shutdown() noexcept = 0;

  // Once Dispatch returns kOwnedSuccess or kOwnedFailure, this backend owns the
  // evaluation. The router must never call another provider for that frame on
  // the same command context.
  virtual BackendResult Dispatch(
      const UniversalFrame& frame,
      const DispatchContext& commands) = 0;
};

}  // namespace renodx::universal
