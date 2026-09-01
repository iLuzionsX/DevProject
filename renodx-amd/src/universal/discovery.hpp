#pragma once

#include <cstdint>
#include <span>

#include "universal_frame.hpp"

namespace renodx::universal {

enum ResourceUsageBits : uint32_t {
  kUsageNone = 0u,
  kUsageRenderTarget = 1u << 0u,
  kUsageDepthStencil = 1u << 1u,
  kUsageShaderRead = 1u << 2u,
  kUsageStorage = 1u << 3u,
  kUsageCopySource = 1u << 4u,
  kUsageCopyDest = 1u << 5u,
  kUsagePresent = 1u << 6u,
};

enum DiscoveryEvidenceBits : uint32_t {
  kEvidenceNone = 0u,
  kEvidenceTaggedByApi = 1u << 0u,
  kEvidenceUsagePattern = 1u << 1u,
  kEvidenceExtentMatch = 1u << 2u,
  kEvidenceFormatMatch = 1u << 3u,
  kEvidenceShaderBinding = 1u << 4u,
  kEvidenceTemporalCorrelation = 1u << 5u,
  kEvidenceMatrixCorrelation = 1u << 6u,
  kEvidenceUserProfile = 1u << 7u,
};

// API adapters populate observations from resource creation, descriptor/binding
// events, render-pass attachments and draw/dispatch history. Discovery can then
// evolve independently of the hook implementation.
struct ResourceObservation {
  ResourceHandle resource{};
  Extent2D extent{};
  uint32_t format = 0u;
  uint32_t usage = kUsageNone;
  uint32_t sample_count = 1u;
  uint64_t first_seen_event = 0u;
  uint64_t last_seen_event = 0u;
  uint32_t frame_write_count = 0u;
  uint32_t frame_read_count = 0u;
  uint32_t shader_bind_count = 0u;
};

struct SemanticCandidate {
  ResourceSemantic semantic = ResourceSemantic::kColor;
  ResourceObservation observation{};
  float score = 0.0f;
  uint32_t evidence = kEvidenceNone;

  [[nodiscard]] constexpr bool IsPlausible(float threshold = 0.5f) const noexcept {
    return observation.resource.IsValid() && score >= threshold;
  }
};

class IResourceClassifier {
 public:
  virtual ~IResourceClassifier() = default;

  // Classifiers must be side-effect free so discovery can run asynchronously
  // over captured metadata and then converge/freeze once a stable mapping is
  // found. The render thread only records observations.
  [[nodiscard]] virtual SemanticCandidate Classify(
      ResourceSemantic semantic,
      const ResourceObservation& observation,
      const UniversalFrame& frame_hint) const noexcept = 0;
};

struct DiscoverySet {
  SemanticCandidate color{};
  SemanticCandidate depth{};
  SemanticCandidate motion_vectors{};
  SemanticCandidate exposure{};
  SemanticCandidate reactive_mask{};
  SemanticCandidate transparency_mask{};
};

}  // namespace renodx::universal
