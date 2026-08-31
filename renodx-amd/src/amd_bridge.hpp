/*
 * Experimental RenoDX AMD upscaler bridge
 *
 * Original integration scaffolding intended to be copied into
 * clshortfuse/renodx/src/utils/dlss/ while the AMD backend is developed.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

#include <sl.h>
#include <sl_dlss.h>

namespace renodx::utils::dlss::amd_bridge {

constexpr uint32_t kLegacyFrameIndex = std::numeric_limits<uint32_t>::max();
constexpr size_t kMaxScopedStates = 64u;

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
  uint32_t frame_index = kLegacyFrameIndex;
  uint32_t viewport = std::numeric_limits<uint32_t>::max();
  FrameResources resources;
  std::optional<sl::Constants> constants;

  [[nodiscard]] bool IsReadyForUpscale() const {
    return resources.HasMinimumUpscaleInputs() && constants.has_value();
  }
};

// Preflight must not record GPU work. It is the final point at which RenoDX may
// safely fall through to the original NVIDIA Streamline implementation.
using PreflightCallback = bool (*)(
    sl::Feature feature,
    const sl::FrameToken& frame,
    const sl::BaseStructure** inputs,
    uint32_t num_inputs,
    sl::CommandBuffer* command_buffer,
    const FrameState& state);

// Once EvaluateOverrideCallback is entered, the alternate backend owns the
// call. Its sl::Result is returned to the game even if dispatch fails; RenoDX
// must not fall through after the backend may have recorded GPU commands.
using EvaluateOverrideCallback = sl::Result (*)(
    sl::Feature feature,
    const sl::FrameToken& frame,
    const sl::BaseStructure** inputs,
    uint32_t num_inputs,
    sl::CommandBuffer* command_buffer,
    const FrameState& state);

namespace internal {

struct FrameKey {
  uint32_t frame_index;
  uint32_t viewport;

  bool operator==(const FrameKey& other) const noexcept {
    return frame_index == other.frame_index && viewport == other.viewport;
  }
};

struct FrameKeyHash {
  size_t operator()(const FrameKey& key) const noexcept {
    const uint64_t packed = (static_cast<uint64_t>(key.frame_index) << 32u)
        | static_cast<uint64_t>(key.viewport);
    return std::hash<uint64_t>{}(packed);
  }
};

struct StoredFrameState {
  FrameState state;
  uint64_t sequence = 0u;
};

inline std::shared_mutex state_mutex;
inline std::unordered_map<FrameKey, StoredFrameState, FrameKeyHash> states;
inline uint64_t next_sequence = 1u;

inline std::atomic<PreflightCallback> preflight_callback = nullptr;
inline std::atomic<EvaluateOverrideCallback> evaluate_callback = nullptr;
inline std::atomic<bool> backend_ready = false;

inline CapturedResource* FindResourceSlot(FrameResources& resources, sl::BufferType type) {
  switch (type) {
    case sl::kBufferTypeDepth:
    case sl::kBufferTypeHiResDepth:
    case sl::kBufferTypeLinearDepth:
      return &resources.depth;
    case sl::kBufferTypeMotionVectors:
      return &resources.motion_vectors;
    case sl::kBufferTypeScalingInputColor:
      return &resources.scaling_input;
    case sl::kBufferTypeScalingOutputColor:
      return &resources.scaling_output;
    case sl::kBufferTypeExposure:
      return &resources.exposure;
    case sl::kBufferTypeReactiveMaskHint:
      return &resources.reactive_mask;
    case sl::kBufferTypeTransparencyAndCompositionMaskHint:
      return &resources.transparency_composition_mask;
    case sl::kBufferTypeHUDLessColor:
      return &resources.hudless_color;
    default:
      return nullptr;
  }
}

inline void ApplyTags(FrameResources& resources, const sl::ResourceTag* tags, uint32_t num_tags) {
  if (tags == nullptr || num_tags == 0u) return;

  for (uint32_t i = 0u; i < num_tags; ++i) {
    const auto& tag = tags[i];
    auto* slot = FindResourceSlot(resources, tag.type);
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

inline void FillMissingResource(CapturedResource& target, const CapturedResource& fallback) {
  if (!target.IsValid() && fallback.IsValid()) target = fallback;
}

inline void MergeMissingResources(FrameResources& target, const FrameResources& fallback) {
  FillMissingResource(target.depth, fallback.depth);
  FillMissingResource(target.motion_vectors, fallback.motion_vectors);
  FillMissingResource(target.scaling_input, fallback.scaling_input);
  FillMissingResource(target.scaling_output, fallback.scaling_output);
  FillMissingResource(target.exposure, fallback.exposure);
  FillMissingResource(target.reactive_mask, fallback.reactive_mask);
  FillMissingResource(target.transparency_composition_mask, fallback.transparency_composition_mask);
  FillMissingResource(target.hudless_color, fallback.hudless_color);
}

inline StoredFrameState& TouchLocked(const FrameKey& key) {
  auto& stored = states[key];
  stored.state.frame_index = key.frame_index;
  stored.state.viewport = key.viewport;
  stored.sequence = next_sequence++;
  return stored;
}

inline void PruneLocked() {
  while (states.size() > kMaxScopedStates) {
    auto oldest = states.end();
    for (auto it = states.begin(); it != states.end(); ++it) {
      if (it->first.frame_index == kLegacyFrameIndex) continue;
      if (oldest == states.end() || it->second.sequence < oldest->second.sequence) oldest = it;
    }
    if (oldest == states.end()) break;
    states.erase(oldest);
  }
}

template <typename T>
inline const T* FindInputStructure(const sl::BaseStructure** inputs, uint32_t num_inputs) {
  if (inputs == nullptr) return nullptr;

  for (uint32_t i = 0u; i < num_inputs; ++i) {
    const sl::BaseStructure* current = inputs[i];
    while (current != nullptr) {
      if (current->structType == T::s_structType) {
        return reinterpret_cast<const T*>(current);
      }
      current = current->next;
    }
  }
  return nullptr;
}

inline void ApplyLocalInputStructures(
    FrameState& state,
    const sl::BaseStructure** inputs,
    uint32_t num_inputs) {
  if (inputs == nullptr) return;

  for (uint32_t i = 0u; i < num_inputs; ++i) {
    const sl::BaseStructure* current = inputs[i];
    while (current != nullptr) {
      if (current->structType == sl::ResourceTag::s_structType) {
        ApplyTags(
            state.resources,
            reinterpret_cast<const sl::ResourceTag*>(current),
            1u);
      } else if (current->structType == sl::Constants::s_structType) {
        state.constants = *reinterpret_cast<const sl::Constants*>(current);
      }
      current = current->next;
    }
  }
}

inline std::optional<FrameState> SnapshotForEvaluation(
    uint32_t frame_index,
    const sl::BaseStructure** inputs,
    uint32_t num_inputs) {
  const auto* viewport_input = FindInputStructure<sl::ViewportHandle>(inputs, num_inputs);
  const std::optional<uint32_t> viewport = viewport_input == nullptr
      ? std::nullopt
      : std::optional<uint32_t>(static_cast<uint32_t>(*viewport_input));

  std::shared_lock lock(state_mutex);

  if (viewport.has_value()) {
    FrameState state;
    state.frame_index = frame_index;
    state.viewport = *viewport;

    const auto exact = states.find(FrameKey{frame_index, *viewport});
    if (exact != states.end()) state = exact->second.state;

    // Legacy slSetTag resources are viewport-scoped but not frame-scoped.
    const auto legacy = states.find(FrameKey{kLegacyFrameIndex, *viewport});
    if (legacy != states.end()) {
      MergeMissingResources(state.resources, legacy->second.state.resources);
      if (!state.constants.has_value() && legacy->second.state.constants.has_value()) {
        state.constants = legacy->second.state.constants;
      }
    }

    lock.unlock();
    ApplyLocalInputStructures(state, inputs, num_inputs);
    return state;
  }

  // Some integrations omit a viewport structure from evaluate inputs. Only
  // accept that case when there is exactly one scoped candidate for the frame;
  // otherwise fail open instead of mixing viewports.
  std::optional<FrameState> unique;
  for (const auto& [key, stored] : states) {
    if (key.frame_index != frame_index) continue;
    if (unique.has_value()) return std::nullopt;
    unique = stored.state;
  }

  lock.unlock();
  if (!unique.has_value()) return std::nullopt;
  ApplyLocalInputStructures(*unique, inputs, num_inputs);
  return unique;
}

}  // namespace internal

inline void CaptureTags(
    const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags,
    uint32_t num_tags) {
  if (tags == nullptr || num_tags == 0u) return;

  std::unique_lock lock(internal::state_mutex);
  auto& stored = internal::TouchLocked(
      internal::FrameKey{kLegacyFrameIndex, static_cast<uint32_t>(viewport)});
  internal::ApplyTags(stored.state.resources, tags, num_tags);
  internal::PruneLocked();
}

inline void CaptureTags(
    const sl::FrameToken& frame,
    const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags,
    uint32_t num_tags) {
  if (tags == nullptr || num_tags == 0u) return;

  std::unique_lock lock(internal::state_mutex);
  auto& stored = internal::TouchLocked(internal::FrameKey{
      static_cast<uint32_t>(frame),
      static_cast<uint32_t>(viewport)});
  internal::ApplyTags(stored.state.resources, tags, num_tags);
  internal::PruneLocked();
}

inline void CaptureConstants(
    const sl::Constants& values,
    const sl::FrameToken& frame,
    const sl::ViewportHandle& viewport) {
  std::unique_lock lock(internal::state_mutex);
  auto& stored = internal::TouchLocked(internal::FrameKey{
      static_cast<uint32_t>(frame),
      static_cast<uint32_t>(viewport)});
  stored.state.constants = values;
  internal::PruneLocked();
}

inline void ResetState() {
  std::unique_lock lock(internal::state_mutex);
  internal::states.clear();
  internal::next_sequence = 1u;
}

inline void SetBackendCallbacks(
    PreflightCallback preflight,
    EvaluateOverrideCallback evaluate) {
  internal::backend_ready.store(false, std::memory_order_release);
  internal::preflight_callback.store(preflight, std::memory_order_release);
  internal::evaluate_callback.store(evaluate, std::memory_order_release);
}

inline void SetBackendReady(bool ready) {
  internal::backend_ready.store(ready, std::memory_order_release);
}

[[nodiscard]] inline bool IsBackendAvailable() {
  return internal::backend_ready.load(std::memory_order_acquire)
      && internal::evaluate_callback.load(std::memory_order_acquire) != nullptr;
}

inline bool TryEvaluate(
    sl::Feature feature,
    const sl::FrameToken& frame,
    const sl::BaseStructure** inputs,
    uint32_t num_inputs,
    sl::CommandBuffer* command_buffer,
    sl::Result* result) {
  if (result == nullptr || feature != sl::kFeatureDLSS || !IsBackendAvailable()) return false;

  auto state = internal::SnapshotForEvaluation(
      static_cast<uint32_t>(frame), inputs, num_inputs);
  if (!state.has_value() || !state->IsReadyForUpscale()) return false;

  const auto preflight = internal::preflight_callback.load(std::memory_order_acquire);
  if (preflight != nullptr
      && !preflight(feature, frame, inputs, num_inputs, command_buffer, *state)) {
    return false;
  }

  const auto evaluate = internal::evaluate_callback.load(std::memory_order_acquire);
  if (evaluate == nullptr) return false;

  *result = evaluate(feature, frame, inputs, num_inputs, command_buffer, *state);
  return true;
}

}  // namespace renodx::utils::dlss::amd_bridge
