#pragma once

#include <array>
#include <cstdint>

#include "input/device.h"

namespace mecvr::input {

// Device-neutral motion-control sample. Positions are OpenXR meters in the
// body frame; the scheme owns no world/player transform.
struct MotionSample {
  std::uint64_t timestamp = 0;
  PoseState head;
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

// Canonical per-hand intent produced from OpenXR poses. This is deliberately
// independent from Catalyst's private input ABI so the same result can feed
// the visible IK layer, the desktop bridge, recordings, and headless tests.
struct MotionIntentHand {
  bool fist = false;
  bool combat_held = false;
  bool combat_pressed = false;
  float combat_strength = 0.0f;
  std::array<float, 3> combat_direction{};
  bool mag_rope_held = false;
  float mag_rope_strength = 0.0f;
  std::array<float, 3> mag_rope_direction{};
};

struct MotionIntentFrame {
  std::uint64_t timestamp = 0;
  Vector2 movement;
  Vector2 turn;
  bool sprint = false;
  bool jump = false;
  bool crouch = false;
  MotionIntentHand left;
  MotionIntentHand right;
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
  float combat_forward_dot = 0.10f;
  std::uint64_t combat_cooldown_ms = 250;
  float rope_pull_start_mps = 0.45f;
  float rope_pull_release_mps = 0.15f;
  float rope_forward_dot = 0.20f;
};

// STRIDE-like locomotion: a non-neutral left stick wins; otherwise an
// alternating forward/back arm swing produces forward movement. The right
// stick remains the turn route. Pose validity is preserved in the output so
// gameplay can choose a fallback without inventing tracking data.
class MotionScheme {
 public:
  explicit MotionScheme(const MotionSchemeConfig& config = {});

  RawFrame Update(const MotionSample& sample);
  const MotionIntentFrame& latestIntent() const { return latest_intent_; }
  void Reset();

 private:
  static float Magnitude(Vector2 v);
  static float Clamp01(float v);
  MotionIntentFrame BuildIntent(const MotionSample& sample, float left_speed,
                                float right_speed, bool alternating,
                                const std::array<float, 3>& left_velocity,
                                const std::array<float, 3>& right_velocity,
                                float dt_seconds);

  MotionSchemeConfig config_;
  MotionSample previous_;
  MotionIntentFrame latest_intent_;
  std::array<std::uint64_t, 2> last_combat_timestamp_{};
  std::array<bool, 2> has_combat_timestamp_{};
  std::array<bool, 2> combat_latched_{};
  std::array<bool, 2> rope_latched_{};
  bool has_previous_ = false;
};

}  // namespace mecvr::input
