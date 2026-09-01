#include <cassert>

#include "universal/backend.hpp"
#include "universal/reconstruction.hpp"
#include "universal/routing.hpp"
#include "universal/universal_frame.hpp"

using namespace renodx::universal;

namespace {

ResourceView MakeResource(ResourceSemantic semantic, float confidence = 1.0f) {
  return ResourceView{
      .resource = ResourceHandle{
          .api = GraphicsApi::kD3D12,
          .native = reinterpret_cast<void*>(0x1),
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
  assert(native_route.use_auto_exposure);

  UniversalFrame camera = native;
  camera.motion_vectors = {};
  camera.depth.confidence = 0.9f;
  camera.camera.confidence = 0.8f;
  camera.camera.provenance = Provenance::kNativeDiscovered;

  const auto camera_route = PlanRoute(camera, backend);
  assert(camera_route.eligible);
  assert(camera_route.tier == InputTier::kCameraReconstructible);
  assert(camera_route.motion.needs_camera_reprojection);
  assert(!camera_route.motion.needs_optical_flow);

  ReconstructionPolicy with_flow{};
  with_flow.optical_flow_available = true;
  const auto hybrid_route = PlanRoute(camera, backend, with_flow);
  assert(hybrid_route.eligible);
  assert(hybrid_route.tier == InputTier::kHybridReconstruction);
  assert(hybrid_route.motion.needs_camera_reprojection);
  assert(hybrid_route.motion.needs_optical_flow);

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

  return 0;
}
