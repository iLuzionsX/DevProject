#pragma once

#include <cstdint>
#include <string_view>

#include "universal_frame.hpp"

namespace renodx::universal {

enum class CaptureStatus : uint8_t {
  kNoFrame = 0,
  kPartial,
  kReady,
};

struct CaptureContext {
  GraphicsApi api = GraphicsApi::kUnknown;
  uint64_t frame_id = 0u;
  void* native_device = nullptr;
  void* native_queue = nullptr;
  void* native_commands = nullptr;
};

// Frontend contract for Streamline, generic D3D12, D3D11, Vulkan, or future
// engine-specific adapters. The adapter only describes what it observed; it
// does not decide which neural/upscaling provider will consume the frame.
class IInputAdapter {
 public:
  virtual ~IInputAdapter() = default;

  [[nodiscard]] virtual std::string_view Name() const noexcept = 0;
  [[nodiscard]] virtual bool Supports(GraphicsApi api) const noexcept = 0;

  virtual CaptureStatus Capture(
      const CaptureContext& context,
      UniversalFrame* frame) = 0;

  virtual void Reset() noexcept = 0;
};

}  // namespace renodx::universal
