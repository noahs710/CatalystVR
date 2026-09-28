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

namespace {

float Dot(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

float Length(const std::array<float, 3>& value) {
  return std::sqrt(Dot(value, value));
}

std::array<float, 3> Normalize(const std::array<float, 3>& value,
                               std::array<float, 3> fallback) {
  const float length = Length(value);
  if (length <= 1e-5f) return fallback;
  return {value[0] / length, value[1] / length, value[2] / length};
}

std::array<float, 3> Rotate(const float q[4], std::array<float, 3> value) {
  const std::array<float, 3> u{q[0], q[1], q[2]};
  const std::array<float, 3> uv{u[1] * value[2] - u[2] * value[1],
                                u[2] * value[0] - u[0] * value[2],
                                u[0] * value[1] - u[1] * value[0]};
  const std::array<float, 3> uuv{u[1] * uv[2] - u[2] * uv[1],
                                 u[2] * uv[0] - u[0] * uv[2],
                                 u[0] * uv[1] - u[1] * uv[0]};
  return {value[0] + 2.0f * (q[3] * uv[0] + uuv[0]),
          value[1] + 2.0f * (q[3] * uv[1] + uuv[1]),
          value[2] + 2.0f * (q[3] * uv[2] + uuv[2])};
}

}  // namespace

MotionIntentFrame MotionScheme::BuildIntent(
    const MotionSample& sample, float left_speed, float right_speed,
    bool alternating, const std::array<float, 3>& left_velocity,
    const std::array<float, 3>& right_velocity, float dt_seconds) {
  MotionIntentFrame intent;
  intent.timestamp = sample.timestamp;
  intent.turn = sample.right_stick;
  intent.jump = sample.jump;
  intent.crouch = sample.crouch;
  const std::array<PoseState, 2> hands{sample.left_hand, sample.right_hand};
  const std::array<std::array<float, 3>, 2> velocities{left_velocity,
                                                        right_velocity};
  const std::array<float, 2> speeds{left_speed, right_speed};
  const std::array<bool, 2> grips{sample.left_grip, sample.right_grip};
  std::array<MotionIntentHand, 2> outputs{};
  for (std::size_t hand = 0; hand < outputs.size(); ++hand) {
    auto& output = outputs[hand];
    const auto& pose = hands[hand];
    const auto velocity = velocities[hand];
    const auto forward = Normalize(Rotate(pose.orientation, {0.0f, 0.0f, -1.0f}),
                                   {0.0f, 0.0f, -1.0f});
    const float forward_speed = Dot(velocity, forward);
    output.fist = grips[hand];
    output.combat_held = output.fist && speeds[hand] >= config_.melee_speed_mps;
    const bool forward_swing =
        speeds[hand] > 1e-5f &&
        (std::fabs(forward_speed) >=
             speeds[hand] * config_.combat_forward_dot ||
         speeds[hand] >= config_.melee_speed_mps * 1.5f);
    output.combat_held = output.combat_held && forward_swing;
    output.combat_strength =
        output.combat_held
            ? Clamp01((speeds[hand] - config_.melee_speed_mps) / 2.0f)
            : 0.0f;
    output.combat_direction = Normalize(velocity, {0.0f, 0.0f, -1.0f});
    if (!output.combat_held) combat_latched_[hand] = false;
    if (output.combat_held && !combat_latched_[hand] &&
        (!has_combat_timestamp_[hand] ||
         sample.timestamp >= last_combat_timestamp_[hand] +
                                 config_.combat_cooldown_ms)) {
      output.combat_pressed = true;
      combat_latched_[hand] = true;
      last_combat_timestamp_[hand] = sample.timestamp;
      has_combat_timestamp_[hand] = true;
    }

    if (pose.valid && sample.head.valid) {
      const std::array<float, 3> to_body{
          sample.head.position[0] - pose.position[0],
          sample.head.position[1] - pose.position[1],
          sample.head.position[2] - pose.position[2]};
      const auto pull_direction = Normalize(to_body, {0.0f, 0.0f, 0.0f});
      const std::array<float, 3> from_body{
          -to_body[0], -to_body[1], -to_body[2]};
      const auto body_forward = Normalize(
          Rotate(sample.head.orientation, {0.0f, 0.0f, -1.0f}),
          {0.0f, 0.0f, -1.0f});
      const float toward_body_speed = Dot(velocity, pull_direction);
      const bool in_forward_cone =
          Dot(Normalize(from_body, body_forward), body_forward) >=
          config_.rope_forward_dot;
      if (!grips[hand] || !in_forward_cone) {
        rope_latched_[hand] = false;
      } else if (toward_body_speed >= config_.rope_pull_start_mps) {
        rope_latched_[hand] = true;
      } else if (toward_body_speed < config_.rope_pull_release_mps &&
                 !rope_latched_[hand]) {
        rope_latched_[hand] = false;
      }
      output.mag_rope_held = rope_latched_[hand];
      output.mag_rope_direction = pull_direction;
      output.mag_rope_strength = output.mag_rope_held
                                     ? Clamp01(toward_body_speed / 2.0f)
                                     : 0.0f;
    } else {
      rope_latched_[hand] = false;
    }
  }
  intent.left = outputs[0];
  intent.right = outputs[1];
  const float swing_speed = 0.5f * (left_speed + right_speed);
  intent.sprint = alternating && swing_speed >= config_.sprint_swing_mps;
  intent.movement = sample.left_stick;
  if (Magnitude(intent.movement) < config_.stick_deadzone && alternating) {
    intent.movement = {0.0f, Clamp01((swing_speed - config_.swing_move_mps) /
                                      (config_.sprint_swing_mps -
                                       config_.swing_move_mps))};
  }
  (void)dt_seconds;
  return intent;
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
  float left_speed = 0.0f;
  float right_speed = 0.0f;
  std::array<float, 3> left_velocity{};
  std::array<float, 3> right_velocity{};
  if (has_previous_ && sample.timestamp > previous_.timestamp) {
    const float dt = static_cast<float>(sample.timestamp - previous_.timestamp) /
                     1000.0f;
    if (dt > 0.001f && dt <= 0.25f) {
      const bool left_valid = sample.left_hand.valid &&
                              previous_.left_hand.valid;
      const bool right_valid = sample.right_hand.valid &&
                               previous_.right_hand.valid;
      if (left_valid) {
        left_velocity = {(sample.left_hand.position[0] -
                          previous_.left_hand.position[0]) /
                             dt,
                         (sample.left_hand.position[1] -
                          previous_.left_hand.position[1]) /
                             dt,
                         (sample.left_hand.position[2] -
                          previous_.left_hand.position[2]) /
                             dt};
      }
      if (right_valid) {
        right_velocity = {(sample.right_hand.position[0] -
                           previous_.right_hand.position[0]) /
                              dt,
                          (sample.right_hand.position[1] -
                           previous_.right_hand.position[1]) /
                              dt,
                          (sample.right_hand.position[2] -
                           previous_.right_hand.position[2]) /
                              dt};
      }
      left_speed = Length(left_velocity);
      right_speed = Length(right_velocity);
      if (left_valid && right_valid) {
        const float lv = left_velocity[2];
        const float rv = right_velocity[2];
        swing_speed = 0.5f * (std::fabs(lv) + std::fabs(rv));
        alternating = lv * rv < -(config_.swing_deadzone_mps *
                                  config_.swing_deadzone_mps);
      }
    }
  }
  latest_intent_ = BuildIntent(sample, left_speed, right_speed, alternating,
                               left_velocity, right_velocity, 0.0f);
  movement = latest_intent_.movement;
  out.values[ActionId::kMove2D] = movement;
  out.values[ActionId::kTurn2D] = sample.right_stick;
  out.values[ActionId::kTriggerLeft] = sample.left_trigger;
  out.values[ActionId::kTriggerRight] = sample.right_trigger;
  out.values[ActionId::kGripLeft] = sample.left_grip;
  out.values[ActionId::kGripRight] = sample.right_grip;
  out.values[ActionId::kSprint] =
      alternating && swing_speed >= config_.sprint_swing_mps;
  out.values[ActionId::kMelee] = latest_intent_.left.combat_pressed ||
                                 latest_intent_.right.combat_pressed;
  out.values[ActionId::kJump] = sample.jump;
  out.values[ActionId::kCrouch] = sample.crouch;
  previous_ = sample;
  has_previous_ = true;
  return out;
}

void MotionScheme::Reset() {
  has_previous_ = false;
  latest_intent_ = {};
  last_combat_timestamp_ = {};
  has_combat_timestamp_ = {};
  combat_latched_ = {};
  rope_latched_ = {};
}

}  // namespace mecvr::input
