#pragma once

#include <cstdint>

#include "input/device.h"

namespace mecvr::input {

// Device-neutral motion-control sample. Positions are OpenXR meters in the
// body frame; the scheme owns no world/player transform.
struct MotionSample {
  std::uint64_t timestamp = 0;
  Vector2 left_stick;
  Vector2 right_stick;
  PoseState left_hand;
  PoseState right_hand;
  bool left_trigger = false;
  bool right_trigger = false;
  bool left_grip = false;
  bool right_grip = false;
  bool jump = false;
  bool crouch = false;
};

// Raised-hands jump gesture shared by the runtime bridge and headless tests.
// Both tracked hands must clear a small head-relative margin.
bool HandsRaisedForJump(double head_y, const PoseState& left,
                        const PoseState& right);

struct MotionSchemeConfig {
  float stick_deadzone = 0.15f;
  float swing_deadzone_mps = 0.15f;
  float swing_move_mps = 0.45f;
  float sprint_swing_mps = 1.35f;
  float melee_speed_mps = 1.8f;
};

// STRIDE-like locomotion: a non-neutral left stick wins; otherwise an
// alternating forward/back arm swing produces forward movement. The right
// stick remains the turn route. Pose validity is preserved in the output so
// gameplay can choose a fallback without inventing tracking data.
class MotionScheme {
 public:
  explicit MotionScheme(const MotionSchemeConfig& config = {});

  RawFrame Update(const MotionSample& sample);
  void Reset();

 private:
  static float Magnitude(Vector2 v);
  static float Clamp01(float v);

  MotionSchemeConfig config_;
  MotionSample previous_;
  bool has_previous_ = false;
};

}  // namespace mecvr::input
