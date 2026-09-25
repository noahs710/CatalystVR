#include "input/arbitration.h"

#include <cmath>
#include <cstring>

namespace mecvr::input {

ActionFamily FamilyOf(ActionId id) {
  switch (id) {
    case ActionId::kHeadPose:
      return ActionFamily::kHead;
    case ActionId::kLeftHandGripPose:
    case ActionId::kLeftHandAimPose:
    case ActionId::kRightHandGripPose:
    case ActionId::kRightHandAimPose:
      return ActionFamily::kPose;
    case ActionId::kMove2D:
    case ActionId::kTurn2D:
    case ActionId::kLook2D:
      return ActionFamily::kLocomotionTurn;
    case ActionId::kTriggerLeft:
    case ActionId::kTriggerRight:
    case ActionId::kGripLeft:
    case ActionId::kGripRight:
    case ActionId::kJump:
    case ActionId::kCrouch:
    case ActionId::kSlide:
    case ActionId::kInteract:
    case ActionId::kMelee:
    case ActionId::kHeavyMelee:
    case ActionId::kAbility:
    case ActionId::kSprint:
    case ActionId::kVault:
    case ActionId::kWallRun:
    case ActionId::kClimb:
    case ActionId::kSwing:
    case ActionId::kRope:
      return ActionFamily::kDigital;
    case ActionId::kMenu:
    case ActionId::kPause:
    case ActionId::kRecenter:
      return ActionFamily::kSystem;
  }
  return ActionFamily::kDigital;
}

bool IsGameDerived(ActionId id) {
  switch (id) {
    case ActionId::kVault:
    case ActionId::kWallRun:
    case ActionId::kClimb:
    case ActionId::kSwing:
    case ActionId::kRope:
      return true;
    default:
      return false;
  }
}

const char* ActionName(ActionId id) {
  switch (id) {
    case ActionId::kHeadPose:
      return "HeadPose";
    case ActionId::kLeftHandGripPose:
      return "LeftHandGripPose";
    case ActionId::kLeftHandAimPose:
      return "LeftHandAimPose";
    case ActionId::kRightHandGripPose:
      return "RightHandGripPose";
    case ActionId::kRightHandAimPose:
      return "RightHandAimPose";
    case ActionId::kMove2D:
      return "Move2D";
    case ActionId::kTurn2D:
      return "Turn2D";
    case ActionId::kLook2D:
      return "Look2D";
    case ActionId::kTriggerLeft:
      return "TriggerL";
    case ActionId::kTriggerRight:
      return "TriggerR";
    case ActionId::kGripLeft:
      return "GripL";
    case ActionId::kGripRight:
      return "GripR";
    case ActionId::kJump:
      return "Jump";
    case ActionId::kCrouch:
      return "Crouch";
    case ActionId::kSlide:
      return "Slide";
    case ActionId::kInteract:
      return "Interact";
    case ActionId::kMelee:
      return "Melee";
    case ActionId::kHeavyMelee:
      return "HeavyMelee";
    case ActionId::kAbility:
      return "Ability";
    case ActionId::kSprint:
      return "Sprint";
    case ActionId::kMenu:
      return "Menu";
    case ActionId::kPause:
      return "Pause";
    case ActionId::kRecenter:
      return "Recenter";
    case ActionId::kVault:
      return "Vault";
    case ActionId::kWallRun:
      return "WallRun";
    case ActionId::kClimb:
      return "Climb";
    case ActionId::kSwing:
      return "Swing";
    case ActionId::kRope:
      return "Rope";
  }
  return "<unknown>";
}

bool TryParseActionName(const char* name, ActionId* out) {
  if (name == nullptr || out == nullptr) return false;
  static const ActionId kAll[] = {
      ActionId::kHeadPose,        ActionId::kLeftHandGripPose,
      ActionId::kLeftHandAimPose, ActionId::kRightHandGripPose,
      ActionId::kRightHandAimPose, ActionId::kMove2D,
      ActionId::kTurn2D,          ActionId::kLook2D,
      ActionId::kTriggerLeft,     ActionId::kTriggerRight,
      ActionId::kGripLeft,        ActionId::kGripRight,
      ActionId::kJump,            ActionId::kCrouch,
      ActionId::kSlide,           ActionId::kInteract,
      ActionId::kMelee,           ActionId::kHeavyMelee,
      ActionId::kAbility,         ActionId::kSprint,
      ActionId::kMenu,            ActionId::kPause,
      ActionId::kRecenter,        ActionId::kVault,
      ActionId::kWallRun,         ActionId::kClimb,
      ActionId::kSwing,           ActionId::kRope,
  };
  for (ActionId id : kAll) {
    if (std::strcmp(name, ActionName(id)) == 0) {
      *out = id;
      return true;
    }
  }
  return false;
}

StickArbiter::StickArbiter(const StickArbiterConfig& config) : config_(config) {}

float StickArbiter::Magnitude(Vector2 v) {
  return std::sqrt(v.x * v.x + v.y * v.y);
}

StickOwner StickArbiter::Update(Vector2 touch, Vector2 xbox) {
  const float touch_mag = Magnitude(touch);
  const float xbox_mag = Magnitude(xbox);
  const float claim = config_.activation_threshold >= 0.0f
                          ? config_.activation_threshold
                          : 0.0f;
  const float quiet = config_.deadzone >= 0.0f ? config_.deadzone : 0.0f;

  auto claim_from_neutral = [&]() {
    const bool touch_claim = touch_mag >= claim;
    const bool xbox_claim = xbox_mag >= claim;
    if (touch_claim && xbox_claim) {
      // Simultaneous claim: larger deflection wins, exact tie goes to
      // Touch. Recorded as an open item in docs/INPUT.md.
      owner_ = (xbox_mag > touch_mag) ? StickOwner::kXbox : StickOwner::kTouch;
    } else if (touch_claim) {
      owner_ = StickOwner::kTouch;
    } else if (xbox_claim) {
      owner_ = StickOwner::kXbox;
    } else {
      owner_ = StickOwner::kNone;
    }
    neutral_frames_ = 0;
  };

  if (owner_ == StickOwner::kNone) {
    claim_from_neutral();
    return owner_;
  }

  // Owned: stays owned while the owner's stick is alive (above deadzone).
  // Stick noise below the deadzone counts as neutral, never as activity.
  const float owned_mag =
      (owner_ == StickOwner::kTouch) ? touch_mag : xbox_mag;
  if (owned_mag > quiet) {
    neutral_frames_ = 0;
    return owner_;
  }
  ++neutral_frames_;
  if (neutral_frames_ > config_.release_hysteresis_frames) {
    // Neutral + hysteresis elapsed: release to unowned first, then let any
    // currently-claiming stick take ownership fresh in this same frame.
    owner_ = StickOwner::kNone;
    claim_from_neutral();
  }
  return owner_;
}

void StickArbiter::Reset() {
  owner_ = StickOwner::kNone;
  neutral_frames_ = 0;
}

const ActionState& DigitalArbiter::Update(bool touch_down, bool xbox_down,
                                          std::uint64_t timestamp) {
  const bool down = touch_down || xbox_down;
  ActionState next;
  next.timestamp = timestamp;
  next.value = ActionValue(down);
  next.held = down;
  next.pressed = down && !state_.held;
  next.released = !down && state_.held;
  if (down) {
    if (!state_.held) {
      // Originator claims attribution. Simultaneous press on both routes
      // attributes Touch; recorded as an open item in docs/INPUT.md.
      // A lone Xbox press (touch_down false) attributes Xbox.
      next.source = touch_down ? ActionSource::kOpenXr : ActionSource::kXInput;
    } else {
      next.source = state_.source;  // Held stays owned by the originator.
    }
  } else {
    next.source =
        state_.held ? state_.source : ActionSource::kNone;  // Attribute release.
  }
  state_ = next;
  return state_;
}

void DigitalArbiter::Reset() { state_ = ActionState(); }

}  // namespace mecvr::input
