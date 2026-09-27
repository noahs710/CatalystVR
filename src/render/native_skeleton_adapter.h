#pragma once

#include <cstddef>
#include <cstdint>

#include "render/bone_palette_classifier.h"

namespace mecvr::render {

// Read-only seam between renderer observations and a future title-specific
// bone-semantic adapter. It deliberately verifies only a stable transform
// palette; it never writes engine buffers or guesses bone names.
struct NativePaletteObservation {
  std::uint64_t present_index = 0;
  std::uint32_t constant_buffer_id = 0;
  std::uint32_t resource_size = 0;
  std::uint64_t content_fingerprint = 0;
  BonePaletteCandidate candidate{};
};

struct NativeSkeletonAdapterSnapshot {
  BonePaletteCandidate candidate{};
  std::uint64_t present_index = 0;
  std::uint32_t constant_buffer_id = 0;
  std::uint32_t resource_size = 0;
  std::uint64_t content_fingerprint = 0;
  std::uint64_t observations = 0;
  std::uint64_t stable_observations = 0;
  bool verified = false;
};

class NativeSkeletonAdapter {
 public:
  explicit NativeSkeletonAdapter(std::size_t minimum_matrices = 24,
                                 std::uint64_t stable_frames = 3);

  // Returns true only when this observation promotes the palette to the
  // verified state. A later mismatch clears verification and starts a new
  // bounded observation window.
  bool observe(const NativePaletteObservation& observation);

  NativeSkeletonAdapterSnapshot snapshot() const;
  void reset();

 private:
  static bool SameLayout(const BonePaletteCandidate& a,
                         const BonePaletteCandidate& b);

  const std::size_t minimum_matrices_;
  const std::uint64_t stable_frames_;
  NativeSkeletonAdapterSnapshot state_{};
  std::uint64_t last_frame_ = 0;
};

}  // namespace mecvr::render
