#pragma once

namespace mecvr::ik {

// Captures the floor plane from the first valid standing sample and keeps it
// stable while the user crouches. Recenter/reset starts a fresh calibration.
class HeightCalibrator {
 public:
  explicit HeightCalibrator(double standing_height = 1.70);

  bool update(double head_y, double* floor_y);
  void reset();
  bool calibrated() const { return calibrated_; }

 private:
  double standing_height_ = 1.70;
  double floor_y_ = 0.0;
  bool calibrated_ = false;
};

bool IsPhysicallyCrouched(double head_y, double floor_y,
                          double standing_height = 1.70);

}  // namespace mecvr::ik
