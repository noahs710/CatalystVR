#pragma once

#include <cstddef>

#include "render/bone_palette_classifier.h"

namespace mecvr::render {

enum class FaithPalettePoseSpace {
  kNone,
  kModelSpace,
  kLocalSpace,
};

struct FaithPaletteMatch {
  bool matched = false;
  FaithPalettePoseSpace pose_space = FaithPalettePoseSpace::kNone;
  double scale = 0.0;
  double score = 0.0;

  explicit operator bool() const { return matched; }
};

// Matches a classified live palette against the retail Faith arm-chain
// geometry. The matcher is deliberately independent of resource identity and
// executable fingerprints: those are separate adapter gates. It proves only
// that the bytes at the known retail indices have the expected bilateral arm
// topology and scale.
FaithPaletteMatch MatchFaithArmPalette(const void* data, std::size_t size,
                                       const BonePaletteCandidate& candidate);

}  // namespace mecvr::render
