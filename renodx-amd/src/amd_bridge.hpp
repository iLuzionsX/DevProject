/*
 * Experimental RenoDX AMD upscaler bridge
 *
 * This file is original integration scaffolding intended to be copied into
 * clshortfuse/renodx/src/utils/dlss/ while the AMD backend is developed.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>

#include <sl.h>
#include <sl_dlss.h>

namespace renodx::utils::dlss::amd_bridge {

struct CapturedResource {
  std::optional<sl::Resource> resource;
  std::optional<sl::Extent> extent;

  [[nodiscard]] bool IsValid() const {
    return resource.has_value() && resource->native != nullptr;
  }
};

struct FrameResources {
  CapturedResource depth;
  CapturedResource motion_vectors;
  CapturedResource scaling_input;
  CapturedResource scaling_output;
  CapturedResource exposure;
  CapturedResource reactive_mask;
  CapturedResource transparency_composition_mask;
  CapturedResource hudless_color;

  [[nodiscard]] bool HasMinimumUpscaleInputs() const {
    return depth.IsValid()
        && motion_vectors.IsValid()
        && scaling_input.IsValid()
        && scaling_output.IsValid();
  }
};

struct FrameState {
  FrameResources resources;
  std::optional<sl::Constants> constants;

  [[nodiscard]] bool IsReadyForUpscale() const {
    return resources.HasMinimumUpscaleInputs() && constants.has_value();
  }
};

// Returns true when the backend consumed the feature call. When true, the
// callback must write the result that RenoDX should return to the game.
using EvaluateOverrideCallback = bool (*)(
    sl::Feature feature,
    const sl::FrameToken& frame,
    const sl::BaseStructure** inputs,
    uint32_t num_inputs,
    sl::CommandBuffer* command_buffer,
    const FrameState& state,
    sl::Result* result);

namespace internal {

inline std::shared_mutex state_mutex;
inline FrameState latest_state;
inline std::atomic<EvaluateOverrideCallback> evaluate_override = nullptr;

inline CapturedResource* FindResourceSlot(sl::BufferType type) {
  switch (type) {
    case sl::kBufferTypeDepth:
    case sl::kBufferTypeHiResDepth:
    case sl::kBufferTypeLinearDepth:
      return &latest_state.resources.depth;
    case sl::kBufferTypeMotionVectors:
      return &latest_state.resources.motion_vectors;
    case sl::kBufferTypeScalingInputColor:
      return &latest_state.resources.scaling_input;
    case sl::kBufferTypeScalingOutputColor:
      return &latest_state.resources.scaling_output;
    case sl::kBufferTypeExposure:
      return &latest_state.resources.exposure;
    case sl::kBufferTypeReactiveMaskHint:
      return &latest_state.resources.reactive_mask;
    case sl::kBufferTypeTransparencyAndCompositionMaskHint:
      return &latest_state.resources.transparency_composition_mask;
    case sl::kBufferTypeHUDLessColor:
      return &latest_state.resources.hudless_color;
    default:
      return nullptr;
  }
}

}  // namespace internal

inline void CaptureTags(const sl::ResourceTag* tags, uint32_t num_tags) {
  if (tags == nullptr || num_tags == 0u) return;

  std::unique_lock lock(internal::state_mutex);

  for (uint32_t i = 0u; i < num_tags; ++i) {
    const auto& tag = tags[i];
    auto* slot = internal::FindResourceSlot(tag.type);
    if (slot == nullptr) continue;

    if (tag.resource == nullptr || tag.resource->native == nullptr) {
      slot->resource.reset();
      slot->extent.reset();
      continue;
    }

    slot->resource = *tag.resource;
    slot->extent = tag.extent;
  }
}

inline void CaptureConstants(const sl::Constants& values) {
  std::unique_lock lock(internal::state_mutex);
  internal::latest_state.constants = values;
}

inline FrameState SnapshotState() {
  std::shared_lock lock(internal::state_mutex);
  return internal::latest_state;
}

inline void ResetState() {
  std::unique_lock lock(internal::state_mutex);
  internal::latest_state = {};
}

inline void SetEvaluateOverride(EvaluateOverrideCallback callback) {
  internal::evaluate_override.store(callback, std::memory_order_release);
}

[[nodiscard]] inline bool IsBackendAvailable() {
  return internal::evaluate_override.load(std::memory_order_acquire) != nullptr;
}

inline bool TryEvaluate(
    sl::Feature feature,
    const sl::FrameToken& frame,
    const sl::BaseStructure** inputs,
    uint32_t num_inputs,
    sl::CommandBuffer* command_buffer,
    sl::Result* result) {
  if (result == nullptr || feature != sl::kFeatureDLSS) return false;

  const auto callback = internal::evaluate_override.load(std::memory_order_acquire);
  if (callback == nullptr) return false;

  const auto state = SnapshotState();
  if (!state.IsReadyForUpscale()) return false;

  return callback(feature, frame, inputs, num_inputs, command_buffer, state, result);
}

}  // namespace renodx::utils::dlss::amd_bridge
