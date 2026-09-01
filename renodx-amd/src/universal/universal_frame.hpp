#pragma once

#include <array>
#include <cstdint>

namespace renodx::universal {

enum class GraphicsApi : uint8_t {
  kUnknown = 0,
  kD3D11,
  kD3D12,
  kVulkan,
};

enum class ResourceSemantic : uint8_t {
  kColor = 0,
  kOutput,
  kDepth,
  kMotionVectors,
  kExposure,
  kReactiveMask,
  kTransparencyMask,
  kHudlessColor,
  kConfidenceMask,
};

enum class Provenance : uint8_t {
  kUnavailable = 0,
  kNativeTagged,
  kNativeDiscovered,
  kReconstructedCamera,
  kOpticalFlow,
  kHybrid,
  kUserProfile,
};

enum class MotionVectorUnits : uint8_t {
  kUnknown = 0,
  kPixels,
  kNormalized,
};

enum class MotionVectorDirection : uint8_t {
  kUnknown = 0,
  kCurrentToPrevious,
  kPreviousToCurrent,
};

enum class DepthConvention : uint8_t {
  kUnknown = 0,
  kZeroToOne,
  kMinusOneToOne,
};

enum class MatrixLayout : uint8_t {
  kUnknown = 0,
  kRowMajor,
  kColumnMajor,
};

struct Extent2D {
  uint32_t width = 0;
  uint32_t height = 0;

  [[nodiscard]] constexpr bool IsValid() const noexcept {
    return width != 0u && height != 0u;
  }
};

// API-neutral identity for a GPU object. `native` is intentionally opaque so
// capture adapters can carry ID3D11Resource*, ID3D12Resource*, VkImage, etc.
// `auxiliary` is reserved for an API-specific view/subresource identifier.
struct ResourceHandle {
  GraphicsApi api = GraphicsApi::kUnknown;
  void* native = nullptr;
  uint64_t auxiliary = 0u;

  [[nodiscard]] constexpr bool IsValid() const noexcept {
    return native != nullptr;
  }
};

struct ResourceView {
  ResourceHandle resource{};
  Extent2D extent{};
  uint32_t x = 0u;
  uint32_t y = 0u;
  uint32_t format = 0u;
  ResourceSemantic semantic = ResourceSemantic::kColor;
  Provenance provenance = Provenance::kUnavailable;
  float confidence = 0.0f;

  [[nodiscard]] constexpr bool IsValid() const noexcept {
    return resource.IsValid() && extent.IsValid();
  }
};

struct MotionMetadata {
  MotionVectorUnits units = MotionVectorUnits::kUnknown;
  MotionVectorDirection direction = MotionVectorDirection::kUnknown;
  float scale_x = 1.0f;
  float scale_y = 1.0f;
  bool contains_jitter = false;
};

struct DepthMetadata {
  DepthConvention convention = DepthConvention::kUnknown;
  bool reversed = false;
  bool infinite_far = false;
  float near_plane = 0.0f;
  float far_plane = 0.0f;
};

struct CameraState {
  std::array<float, 16> view_projection{};
  std::array<float, 16> previous_view_projection{};
  std::array<float, 16> inverse_view_projection{};
  // Direct current-clip -> previous-clip transform when the capture API
  // provides it. This is enough for camera-motion reprojection without
  // reconstructing world-space camera matrices first.
  std::array<float, 16> clip_to_previous_clip{};
  MatrixLayout layout = MatrixLayout::kUnknown;
  float jitter_x = 0.0f;
  float jitter_y = 0.0f;
  float vertical_fov_radians = 0.0f;
  float confidence = 0.0f;
  Provenance provenance = Provenance::kUnavailable;
};

struct FrameTiming {
  uint64_t frame_id = 0u;
  double delta_ms = 0.0;
  bool reset = false;
  bool camera_cut = false;
};

// The provider-neutral contract between capture/reconstruction and rendering
// backends. Capture adapters may leave optional resources invalid; the router
// decides whether a backend can run directly, after reconstruction, or not at
// all. This keeps inference/backend code independent from Streamline/RenoDX.
struct UniversalFrame {
  GraphicsApi api = GraphicsApi::kUnknown;
  FrameTiming timing{};
  Extent2D render_extent{};
  Extent2D output_extent{};

  ResourceView color{};
  ResourceView output{};
  ResourceView depth{};
  ResourceView motion_vectors{};
  ResourceView exposure{};
  ResourceView reactive_mask{};
  ResourceView transparency_mask{};
  ResourceView hudless_color{};
  ResourceView confidence_mask{};

  MotionMetadata motion{};
  DepthMetadata depth_info{};
  CameraState camera{};

  float pre_exposure = 1.0f;
  bool hdr = false;

  [[nodiscard]] constexpr bool HasRequiredSurfaces() const noexcept {
    return color.IsValid() && output.IsValid();
  }

  [[nodiscard]] constexpr bool HasNativeTemporalCore() const noexcept {
    return HasRequiredSurfaces() && depth.IsValid() && motion_vectors.IsValid();
  }

  [[nodiscard]] constexpr bool HasReconstructibleCameraMotion() const noexcept {
    return HasRequiredSurfaces() && depth.IsValid() && camera.confidence > 0.0f;
  }
};

}  // namespace renodx::universal
