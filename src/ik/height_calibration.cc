#include "ik/height_calibration.h"

#include <cmath>

namespace mecvr::ik {

HeightCalibrator::HeightCalibrator(double standing_height)
    : standing_height_(std::isfinite(standing_height) && standing_height > 0.1
                           ? standing_height
                           : 1.70) {}

bool HeightCalibrator::update(double head_y, double* floor_y) {
  if (floor_y == nullptr || !std::isfinite(head_y)) return false;
  if (!calibrated_) {
    floor_y_ = head_y - standing_height_;
    calibrated_ = true;
  }
  *floor_y = floor_y_;
  return true;
}

void HeightCalibrator::reset() {
  floor_y_ = 0.0;
  calibrated_ = false;
}

bool IsPhysicallyCrouched(double head_y, double floor_y,
                          double standing_height) {
  return std::isfinite(head_y) && std::isfinite(floor_y) &&
         std::isfinite(standing_height) && standing_height > 0.1 &&
         head_y - floor_y < standing_height * 0.78;
}

}  // namespace mecvr::ik
