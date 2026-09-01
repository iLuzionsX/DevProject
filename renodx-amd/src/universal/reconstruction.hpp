#pragma once

#include <algorithm>
#include <cstdint>

#include "universal_frame.hpp"

namespace renodx::universal {

enum class MotionSource : uint8_t {
  kUnavailable = 0,
  kNative,
  kNativeWithCameraReprojection,
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
  bool needs_native_motion = false;
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
  const bool native_usable = frame.motion_vectors.IsValid()
      && frame.motion_vectors.confidence >= policy.minimum_native_motion_confidence;
  const bool native_camera_complete =
      frame.motion.camera_motion != CameraMotionCoverage::kMissing;

  if (native_usable && native_camera_complete) {
    return MotionPlan{
        .source = MotionSource::kNative,
        .expected_confidence = ClampConfidence(frame.motion_vectors.confidence),
        .needs_native_motion = true,
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

  // Streamline explicitly supports native object motion without camera motion.
  // Preserve those native vectors and fill only missing/invalid pixels from the
  // camera reprojection path rather than discarding object motion wholesale.
  //
  // Do not merge a jittered native field with our unjittered camera reprojection:
  // FidelityFX's jitter-cancellation flag applies to the whole field. Until the
  // completion pass tracks previous jitter and normalizes native vectors itself,
  // fall through to camera-only reconstruction for that mixed-domain case.
  if (native_usable
      && frame.motion.camera_motion == CameraMotionCoverage::kMissing
      && !frame.motion.contains_jitter
      && camera_usable) {
    return MotionPlan{
        .source = MotionSource::kNativeWithCameraReprojection,
        .expected_confidence = ClampConfidence(std::min(
            frame.motion_vectors.confidence,
            std::min(frame.depth.confidence, frame.camera.confidence))),
        .needs_native_motion = true,
        .needs_camera_reprojection = true,
        .needs_optical_flow = false,
        .requires_history = true,
    };
  }

  if (camera_usable && policy.optical_flow_available
      && policy.prefer_hybrid_for_reconstructed_motion) {
    const float camera_score = std::min(frame.depth.confidence, frame.camera.confidence);
    return MotionPlan{
        .source = MotionSource::kHybridCameraOpticalFlow,
        .expected_confidence = ClampConfidence(0.5f + 0.5f * camera_score),
        .needs_native_motion = false,
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
        .needs_native_motion = false,
        .needs_camera_reprojection = true,
        .needs_optical_flow = false,
        .requires_history = true,
    };
  }

  if (history_usable && policy.optical_flow_available && frame.color.IsValid()) {
    return MotionPlan{
        .source = MotionSource::kOpticalFlow,
        .expected_confidence = 0.5f,
        .needs_native_motion = false,
        .needs_camera_reprojection = false,
        .needs_optical_flow = true,
        .requires_history = true,
    };
  }

  return {};
}

}  // namespace renodx::universal
