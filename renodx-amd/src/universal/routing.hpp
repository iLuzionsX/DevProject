#pragma once

#include <cstdint>

#include "backend.hpp"
#include "reconstruction.hpp"

namespace renodx::universal {

enum class InputTier : uint8_t {
  kInsufficient = 0,
  kNativeTemporal,
  kNativeCameraCompletion,
  kCameraReconstructible,
  kOpticalFlowFallback,
  kHybridReconstruction,
};

struct RouteDecision {
  bool eligible = false;
  InputTier tier = InputTier::kInsufficient;
  MotionPlan motion{};
  bool use_auto_exposure = false;
  bool use_reactive_mask = false;
  bool use_transparency_mask = false;
};

[[nodiscard]] inline RouteDecision PlanRoute(
    const UniversalFrame& frame,
    const BackendCapabilities& backend,
    const ReconstructionPolicy& reconstruction = {}) noexcept {
  RouteDecision decision{};

  if (!frame.HasRequiredSurfaces() || !backend.SupportsApi(frame.api)) return decision;
  if (frame.hdr && !backend.supports_hdr) return decision;
  if (backend.requires_depth && !frame.depth.IsValid()) return decision;

  decision.motion = PlanMotion(frame, reconstruction);
  if (backend.requires_motion_vectors && !decision.motion.IsUsable()) return decision;
  if (backend.requires_motion_vectors
      && decision.motion.source != MotionSource::kNative
      && !backend.accepts_reconstructed_motion) {
    return decision;
  }

  switch (decision.motion.source) {
    case MotionSource::kNative:
      decision.tier = InputTier::kNativeTemporal;
      break;
    case MotionSource::kNativeWithCameraReprojection:
      decision.tier = InputTier::kNativeCameraCompletion;
      break;
    case MotionSource::kCameraReprojection:
      decision.tier = InputTier::kCameraReconstructible;
      break;
    case MotionSource::kOpticalFlow:
      decision.tier = InputTier::kOpticalFlowFallback;
      break;
    case MotionSource::kHybridCameraOpticalFlow:
      decision.tier = InputTier::kHybridReconstruction;
      break;
    default:
      // A backend that does not require motion vectors can still run with only
      // required surfaces/depth.
      if (backend.requires_motion_vectors) return decision;
      decision.tier = InputTier::kNativeTemporal;
      break;
  }

  decision.use_auto_exposure = backend.supports_auto_exposure && !frame.exposure.IsValid();
  decision.use_reactive_mask = backend.supports_reactive_mask && frame.reactive_mask.IsValid();
  decision.use_transparency_mask =
      backend.supports_transparency_mask && frame.transparency_mask.IsValid();
  decision.eligible = true;
  return decision;
}

}  // namespace renodx::universal
