#include "input/motion_scheme.h"

#include <cmath>

namespace mecvr::input {

bool HandsRaisedForJump(double head_y, const PoseState& left,
                        const PoseState& right) {
  if (!std::isfinite(head_y) || !left.valid || !right.valid) return false;
  return left.position[1] > head_y + 0.05f &&
         right.position[1] > head_y + 0.05f;
}

MotionScheme::MotionScheme(const MotionSchemeConfig& config) : config_(config) {}

float MotionScheme::Magnitude(Vector2 v) {
  return std::sqrt(v.x * v.x + v.y * v.y);
}

float MotionScheme::Clamp01(float v) {
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

RawFrame MotionScheme::Update(const MotionSample& sample) {
  RawFrame out;
  out.timestamp = sample.timestamp;
  out.connected = sample.left_hand.valid || sample.right_hand.valid;
  out.poses[ActionId::kLeftHandGripPose] = sample.left_hand;
  out.poses[ActionId::kRightHandGripPose] = sample.right_hand;

  Vector2 movement = sample.left_stick;
  float swing_speed = 0.0f;
  bool alternating = false;
  bool melee = false;
  if (has_previous_ && sample.timestamp > previous_.timestamp) {
    const float dt = static_cast<float>(sample.timestamp - previous_.timestamp) /
                     1000.0f;
    if (dt > 0.001f && dt <= 0.25f && sample.left_hand.valid &&
        sample.right_hand.valid && previous_.left_hand.valid &&
        previous_.right_hand.valid) {
      const float lv = (sample.left_hand.position[2] -
                        previous_.left_hand.position[2]) /
                       dt;
      const float rv = (sample.right_hand.position[2] -
                        previous_.right_hand.position[2]) /
                       dt;
      const float la = std::fabs(lv);
      const float ra = std::fabs(rv);
      swing_speed = 0.5f * (la + ra);
      alternating = lv * rv < -(config_.swing_deadzone_mps *
                                config_.swing_deadzone_mps);
      melee = la >= config_.melee_speed_mps || ra >= config_.melee_speed_mps;
    }
  }
  if (Magnitude(movement) < config_.stick_deadzone && alternating) {
    movement = {0.0f, Clamp01((swing_speed - config_.swing_move_mps) /
                              (config_.sprint_swing_mps -
                               config_.swing_move_mps))};
  }
  out.values[ActionId::kMove2D] = movement;
  out.values[ActionId::kTurn2D] = sample.right_stick;
  out.values[ActionId::kTriggerLeft] = sample.left_trigger;
  out.values[ActionId::kTriggerRight] = sample.right_trigger;
  out.values[ActionId::kGripLeft] = sample.left_grip;
  out.values[ActionId::kGripRight] = sample.right_grip;
  out.values[ActionId::kSprint] =
      alternating && swing_speed >= config_.sprint_swing_mps;
  out.values[ActionId::kMelee] = melee;
  out.values[ActionId::kJump] = sample.jump;
  out.values[ActionId::kCrouch] = sample.crouch;
  previous_ = sample;
  has_previous_ = true;
  return out;
}

void MotionScheme::Reset() { has_previous_ = false; }

}  // namespace mecvr::input
