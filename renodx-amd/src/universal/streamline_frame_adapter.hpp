#pragma once

#include "../amd_bridge.hpp"
#include "universal_frame.hpp"

namespace renodx::universal::streamline {

using CapturedResource = renodx::utils::dlss::amd_bridge::CapturedResource;
using FrameState = renodx::utils::dlss::amd_bridge::FrameState;

struct FrameExtents {
  Extent2D render{};
  Extent2D output{};
  Extent2D depth{};
  Extent2D motion{};
};

[[nodiscard]] inline Extent2D ResolveExtent(
    const CapturedResource& captured,
    Extent2D fallback = {}) noexcept {
  if (captured.extent.has_value() && static_cast<bool>(*captured.extent)) {
    return Extent2D{captured.extent->width, captured.extent->height};
  }
  return fallback;
}

[[nodiscard]] inline ResourceView ToResourceView(
    const CapturedResource& captured,
    ResourceSemantic semantic,
    Extent2D fallback = {}) noexcept {
  ResourceView view{};
  view.semantic = semantic;
  if (!captured.IsValid()) return view;

  view.resource.api = GraphicsApi::kD3D12;
  view.resource.native = captured.resource->native;
  view.extent = ResolveExtent(captured, fallback);
  view.provenance = Provenance::kNativeTagged;
  view.confidence = 1.0f;
  return view;
}

// Converts the already-captured Streamline/DLSS contract into the provider-
// neutral frame ABI. This is adapter #1: no discovery or reconstruction is
// performed here, and existing backend behavior remains authoritative.
[[nodiscard]] inline UniversalFrame ToUniversalFrame(
    const FrameState& state,
    const FrameExtents& extents) noexcept {
  UniversalFrame frame{};
  frame.api = GraphicsApi::kD3D12;
  frame.timing.frame_id = state.frame_index;
  frame.render_extent = extents.render;
  frame.output_extent = extents.output;

  frame.color = ToResourceView(
      state.resources.scaling_input,
      ResourceSemantic::kColor,
      extents.render);
  frame.output = ToResourceView(
      state.resources.scaling_output,
      ResourceSemantic::kOutput,
      extents.output);
  frame.depth = ToResourceView(
      state.resources.depth,
      ResourceSemantic::kDepth,
      extents.depth.IsValid() ? extents.depth : extents.render);
  frame.motion_vectors = ToResourceView(
      state.resources.motion_vectors,
      ResourceSemantic::kMotionVectors,
      extents.motion);
  frame.exposure = ToResourceView(
      state.resources.exposure,
      ResourceSemantic::kExposure);
  frame.reactive_mask = ToResourceView(
      state.resources.reactive_mask,
      ResourceSemantic::kReactiveMask,
      extents.render);
  frame.transparency_mask = ToResourceView(
      state.resources.transparency_composition_mask,
      ResourceSemantic::kTransparencyMask,
      extents.render);
  frame.hudless_color = ToResourceView(
      state.resources.hudless_color,
      ResourceSemantic::kHudlessColor,
      extents.output);

  if (state.constants.has_value()) {
    const auto& constants = *state.constants;
    frame.timing.reset = constants.reset == sl::Boolean::eTrue;
    frame.camera.jitter_x = constants.jitterOffset.x;
    frame.camera.jitter_y = constants.jitterOffset.y;
    frame.camera.vertical_fov_radians = constants.cameraFOV;

    frame.depth_info.convention = DepthConvention::kZeroToOne;
    frame.depth_info.reversed = constants.depthInverted == sl::Boolean::eTrue;
    frame.depth_info.near_plane = constants.cameraNear;
    frame.depth_info.far_plane = constants.cameraFar;

    // Streamline's mvecScale converts the stored motion-vector values into its
    // normalized motion domain. Backends can then convert that domain to their
    // own pixel/texture convention using the actual MV resource extent.
    frame.motion.units = MotionVectorUnits::kNormalized;
    frame.motion.direction = MotionVectorDirection::kCurrentToPrevious;
    frame.motion.scale_x = constants.mvecScale.x;
    frame.motion.scale_y = constants.mvecScale.y;
    frame.motion.contains_jitter =
        constants.motionVectorsJittered == sl::Boolean::eTrue;
  }

  if (state.dlss_options.has_value()) {
    const auto& options = *state.dlss_options;
    frame.hdr = options.colorBuffersHDR == sl::Boolean::eTrue;
    if (options.preExposure > 0.0f) frame.pre_exposure = options.preExposure;
  }

  return frame;
}

}  // namespace renodx::universal::streamline
