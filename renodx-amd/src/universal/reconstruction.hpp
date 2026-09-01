#pragma once

#include <algorithm>
#include <cstdint>

#include "universal_frame.hpp"

namespace renodx::universal {

enum class MotionSource : uint8_t {
  kUnavailable = 0,
  kNative,
  kCameraReprojection,
  kOpticalFlow,
  kHybridCameraOpticalFlow,
};

struct ReconstructionPolicy {
  float minimum_native_motion_confidence = 0.75f;
  float minimum_camera_confidence = 0.60f;
  float minimum_depth_confidence = 0.60f;
  bool optical_flow_available = false;
  bool prefer_hybrid_for_reconstructed_motion = true;
};

struct MotionPlan {
  MotionSource source = MotionSource::kUnavailable;
  float expected_confidence = 0.0f;
  bool needs_camera_reprojection = false;
  bool needs_optical_flow = false;
  bool requires_history = false;

  [[nodiscard]] constexpr bool IsUsable() const noexcept {
    return source != MotionSource::kUnavailable;
  }
};

[[nodiscard]] constexpr float ClampConfidence(float value) noexcept {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

[[nodiscard]] inline MotionPlan PlanMotion(
    const UniversalFrame& frame,
    const ReconstructionPolicy& policy = {}) noexcept {
  if (frame.motion_vectors.IsValid()
      && frame.motion_vectors.confidence >= policy.minimum_native_motion_confidence) {
    return MotionPlan{
        .source = MotionSource::kNative,
        .expected_confidence = ClampConfidence(frame.motion_vectors.confidence),
        .needs_camera_reprojection = false,
        .needs_optical_flow = false,
        .requires_history = false,
    };
  }

  // Reconstructed motion cannot cross a history discontinuity. Native vectors
  // remain eligible above because the consumer can still receive the frame's
  // explicit reset signal while using the game's current resources.
  const bool history_usable = !frame.timing.reset && !frame.timing.camera_cut;
  const bool camera_usable = history_usable
      && frame.depth.IsValid()
      && frame.depth.confidence >= policy.minimum_depth_confidence
      && frame.camera.confidence >= policy.minimum_camera_confidence;

  if (camera_usable && policy.optical_flow_available
      && policy.prefer_hybrid_for_reconstructed_motion) {
    const float camera_score = std::min(frame.depth.confidence, frame.camera.confidence);
    return MotionPlan{
        .source = MotionSource::kHybridCameraOpticalFlow,
        .expected_confidence = ClampConfidence(0.5f + 0.5f * camera_score),
        .needs_camera_reprojection = true,
        .needs_optical_flow = true,
        .requires_history = true,
    };
  }

  if (camera_usable) {
    return MotionPlan{
        .source = MotionSource::kCameraReprojection,
        .expected_confidence = ClampConfidence(
            std::min(frame.depth.confidence, frame.camera.confidence)),
        .needs_camera_reprojection = true,
        .needs_optical_flow = false,
        .requires_history = true,
    };
  }

  if (history_usable && policy.optical_flow_available && frame.color.IsValid()) {
    return MotionPlan{
        .source = MotionSource::kOpticalFlow,
        .expected_confidence = 0.5f,
        .needs_camera_reprojection = false,
        .needs_optical_flow = true,
        .requires_history = true,
    };
  }

  return {};
}

}  // namespace renodx::universal
