#pragma once

#include <cstdint>

namespace mecvr::render {

struct CropUv {
  float origin_x = 0.0f;
  float origin_y = 0.0f;
  float extent_x = 0.0f;
  float extent_y = 0.0f;
  bool valid = false;
};

// Returns the normalized source rectangle that fills the destination while
// preserving aspect ratio. The destination is never letterboxed.
inline CropUv CenterCropUv(std::uint32_t source_width,
                           std::uint32_t source_height,
                           std::uint32_t target_width,
                           std::uint32_t target_height) {
  if (source_width == 0 || source_height == 0 || target_width == 0 ||
      target_height == 0) {
    return {};
  }

  const double scale_x = static_cast<double>(target_width) / source_width;
  const double scale_y = static_cast<double>(target_height) / source_height;
  const double scale = scale_x > scale_y ? scale_x : scale_y;
  const double visible_width = static_cast<double>(target_width) / scale;
  const double visible_height = static_cast<double>(target_height) / scale;

  CropUv crop;
  crop.origin_x = static_cast<float>(
      (static_cast<double>(source_width) - visible_width) /
      (2.0 * source_width));
  crop.origin_y = static_cast<float>(
      (static_cast<double>(source_height) - visible_height) /
      (2.0 * source_height));
  crop.extent_x = static_cast<float>(visible_width / source_width);
  crop.extent_y = static_cast<float>(visible_height / source_height);
  crop.valid = true;
  return crop;
}

}  // namespace mecvr::render
