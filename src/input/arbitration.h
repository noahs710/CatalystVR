#pragma once

// Per-family arbitration per spec section 13. Family ownership is
// independent: each family arbiter instance owns only its own family, so
// e.g. Xbox can own locomotion while Touch owns turning.
#include "input/actions.h"

namespace mecvr::input {

// HEAD is always HMD when VR is active. No arbitration, no fallback.
inline ActionSource HeadSource() { return ActionSource::kOpenXr; }

// POSE is Touch when tracked and never stolen by Xbox stick activity.
// Returns kNone when untracked (Xbox never owns a pose).
inline ActionSource PoseOwner(bool touch_tracked) {
  return touch_tracked ? ActionSource::kOpenXr : ActionSource::kNone;
}

enum class StickOwner { kNone, kTouch, kXbox };

inline ActionSource StickOwnerSource(StickOwner owner) {
  switch (owner) {
    case StickOwner::kTouch:
      return ActionSource::kOpenXr;
    case StickOwner::kXbox:
      return ActionSource::kXInput;
    case StickOwner::kNone:
      return ActionSource::kNone;
  }
  return ActionSource::kNone;
}

struct StickArbiterConfig {
  float activation_threshold = 0.35f;  // Magnitude that claims ownership.
  float deadzone = 0.15f;              // Below this the stick is neutral.
  int release_hysteresis_frames = 6;  // Neutral frames before unowned.
};

// LOCOMOTION/TURN arbiter between Xbox and Touch sticks:
// inactive -> exceeds threshold -> owned -> stays owned while active->
// neutral + hysteresis -> unowned. Sub-deadzone noise never claims
// ownership, and a challenger cannot steal during hysteresis: ownership
// returns to unowned first, then the challenger claims fresh.
class StickArbiter {
 public:
  explicit StickArbiter(const StickArbiterConfig& config = StickArbiterConfig());

  // One call per frame with the raw stick vectors of both routes.
  StickOwner Update(Vector2 touch, Vector2 xbox);
  StickOwner owner() const { return owner_; }
  void Reset();

 private:
  static float Magnitude(Vector2 v);

  StickArbiterConfig config_;
  StickOwner owner_ = StickOwner::kNone;
  int neutral_frames_ = 0;
};

// DIGITAL merge: presses from both devices merge; while held, the action
// stays attributed to the originating source until fully released (both
// routes neutral). A release from a non-originator changes nothing.
// Used for discrete intents; SYSTEM actions (menu/recenter) reuse this
// with the either-route rule.
class DigitalArbiter {
 public:
  const ActionState& state() const { return state_; }

  // One call per frame with the per-route down levels.
  const ActionState& Update(bool touch_down, bool xbox_down,
                            std::uint64_t timestamp);
  void Reset();

 private:
  ActionState state_;
};

}  // namespace mecvr::input
