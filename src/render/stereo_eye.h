#pragma once

#include <algorithm>
#include <cmath>

namespace mecvr::render {

enum class StereoEye { kUnknown = -1, kLeft = 0, kRight = 1 };

struct StereoViewport {
  double left = 0.0;
  double width = 0.0;
};

// Classify a side-by-side per-eye viewport without guessing for a full-frame
// viewport. The tolerance covers fractional viewport coordinates while still
// rejecting arbitrary UI/scissor rectangles.
inline StereoEye ClassifyStereoViewport(StereoViewport viewport,
                                         double target_width) {
  if (!(target_width > 0.0) || !(viewport.width > 0.0) ||
      viewport.width > target_width * 0.60) {
    return StereoEye::kUnknown;
  }
  const double half = target_width * 0.5;
  const double tolerance = (std::max)(1.0, target_width * 0.02);
  if (std::abs(viewport.width - half) > tolerance) {
    return StereoEye::kUnknown;
  }
  if (std::abs(viewport.left) <= tolerance) return StereoEye::kLeft;
  if (std::abs(viewport.left - half) <= tolerance) return StereoEye::kRight;
  return StereoEye::kUnknown;
}

}  // namespace mecvr::render
