#include <cassert>

#include "universal/backend.hpp"
#include "universal/capture.hpp"
#include "universal/discovery.hpp"
#include "universal/reconstruction.hpp"
#include "universal/routing.hpp"
#include "universal/universal_frame.hpp"

using namespace renodx::universal;

namespace {

int dummy_resource = 0;

ResourceView MakeResource(ResourceSemantic semantic, float confidence = 1.0f) {
  return ResourceView{
      .resource = ResourceHandle{
          .api = GraphicsApi::kD3D12,
          .native = &dummy_resource,
      },
      .extent = Extent2D{1920u, 1080u},
      .semantic = semantic,
      .provenance = Provenance::kNativeTagged,
      .confidence = confidence,
  };
}

BackendCapabilities Dx12TemporalBackend() {
  return BackendCapabilities{
      .graphics_api_mask = GraphicsApiBit(GraphicsApi::kD3D12),
      .requires_depth = true,
      .requires_motion_vectors = true,
      .accepts_reconstructed_motion = true,
      .supports_auto_exposure = true,
      .supports_reactive_mask = true,
      .supports_transparency_mask = true,
      .supports_hdr = true,
  };
}

}  // namespace

int main() {
  static_assert(GraphicsApiBit(GraphicsApi::kD3D11) != 0u);
  static_assert(GraphicsApiBit(GraphicsApi::kD3D12) != 0u);
  static_assert(GraphicsApiBit(GraphicsApi::kVulkan) != 0u);
  static_assert(GraphicsApiBit(GraphicsApi::kUnknown) == 0u);

  UniversalFrame native{};
  native.api = GraphicsApi::kD3D12;
  native.render_extent = {1920u, 1080u};
  native.output_extent = {2560u, 1440u};
  native.color = MakeResource(ResourceSemantic::kColor);
  native.output = MakeResource(ResourceSemantic::kOutput);
  native.depth = MakeResource(ResourceSemantic::kDepth);
  native.motion_vectors = MakeResource(ResourceSemantic::kMotionVectors);

  const auto backend = Dx12TemporalBackend();
  const auto native_route = PlanRoute(native, backend);
  assert(native_route.eligible);
  assert(native_route.tier == InputTier::kNativeTemporal);
  assert(native_route.motion.source == MotionSource::kNative);
  assert(native_route.motion.needs_native_motion);
  assert(native_route.use_auto_exposure);

  UniversalFrame camera = native;
  camera.motion_vectors = {};
  camera.depth.confidence = 0.9f;
  camera.camera.confidence = 0.8f;
  camera.camera.provenance = Provenance::kNativeDiscovered;
  camera.camera.layout = MatrixLayout::kRowMajor;
  camera.camera.clip_to_previous_clip[0] = 1.0f;
  camera.camera.clip_to_previous_clip[5] = 1.0f;
  camera.camera.clip_to_previous_clip[10] = 1.0f;
  camera.camera.clip_to_previous_clip[15] = 1.0f;

  const auto camera_route = PlanRoute(camera, backend);
  assert(camera_route.eligible);
  assert(camera_route.tier == InputTier::kCameraReconstructible);
  assert(!camera_route.motion.needs_native_motion);
  assert(camera_route.motion.needs_camera_reprojection);
  assert(!camera_route.motion.needs_optical_flow);

  UniversalFrame incomplete_native = camera;
  incomplete_native.motion_vectors = MakeResource(ResourceSemantic::kMotionVectors);
  incomplete_native.motion.camera_motion = CameraMotionCoverage::kMissing;
  const auto completion_route = PlanRoute(incomplete_native, backend);
  assert(completion_route.eligible);
  assert(completion_route.tier == InputTier::kNativeCameraCompletion);
  assert(completion_route.motion.source == MotionSource::kNativeWithCameraReprojection);
  assert(completion_route.motion.needs_native_motion);
  assert(completion_route.motion.needs_camera_reprojection);
  assert(!completion_route.motion.needs_optical_flow);

  UniversalFrame jittered_incomplete_native = incomplete_native;
  jittered_incomplete_native.motion.contains_jitter = true;
  const auto jittered_completion_route = PlanRoute(jittered_incomplete_native, backend);
  assert(jittered_completion_route.eligible);
  assert(jittered_completion_route.tier == InputTier::kCameraReconstructible);
  assert(jittered_completion_route.motion.source == MotionSource::kCameraReprojection);
  assert(!jittered_completion_route.motion.needs_native_motion);
  assert(jittered_completion_route.motion.needs_camera_reprojection);

  UniversalFrame reset_completion = incomplete_native;
  reset_completion.timing.reset = true;
  const auto reset_completion_route = PlanRoute(reset_completion, backend);
  assert(!reset_completion_route.eligible);

  UniversalFrame reset_camera = camera;
  reset_camera.timing.reset = true;
  const auto reset_route = PlanRoute(reset_camera, backend);
  assert(!reset_route.eligible);

  ReconstructionPolicy with_flow{};
  with_flow.optical_flow_available = true;
  const auto hybrid_route = PlanRoute(camera, backend, with_flow);
  assert(hybrid_route.eligible);
  assert(hybrid_route.tier == InputTier::kHybridReconstruction);
  assert(hybrid_route.motion.needs_camera_reprojection);
  assert(hybrid_route.motion.needs_optical_flow);

  const auto reset_hybrid_route = PlanRoute(reset_camera, backend, with_flow);
  assert(!reset_hybrid_route.eligible);

  UniversalFrame color_only{};
  color_only.api = GraphicsApi::kD3D12;
  color_only.color = MakeResource(ResourceSemantic::kColor);
  color_only.output = MakeResource(ResourceSemantic::kOutput);

  const auto insufficient = PlanRoute(color_only, backend, with_flow);
  assert(!insufficient.eligible);  // Depth is still required by this backend.

  UniversalFrame wrong_api = native;
  wrong_api.api = GraphicsApi::kVulkan;
  const auto rejected_api = PlanRoute(wrong_api, backend);
  assert(!rejected_api.eligible);

  ResourceObservation observation{};
  observation.resource = native.depth.resource;
  observation.extent = native.depth.extent;
  observation.usage = kUsageDepthStencil | kUsageShaderRead;
  observation.frame_write_count = 1u;
  observation.frame_read_count = 2u;

  SemanticCandidate candidate{};
  candidate.semantic = ResourceSemantic::kDepth;
  candidate.observation = observation;
  candidate.score = 0.9f;
  candidate.evidence = kEvidenceUsagePattern | kEvidenceExtentMatch;
  assert(candidate.IsPlausible());

  CaptureContext capture{};
  capture.api = GraphicsApi::kD3D12;
  capture.frame_id = 42u;
  assert(capture.api == GraphicsApi::kD3D12);
  assert(capture.frame_id == 42u);

  return 0;
}
