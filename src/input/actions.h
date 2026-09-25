#pragma once

// T4 input interfaces: layered action taxonomy per spec section 12.
// Inputs (what devices produce) and gameplay outcomes (what MEC decides)
// are distinct: derived MEC actions are declared here so gameplay code can
// name them, but the input stream must NEVER produce them.
//
// Only Recenter, diagnostics, and test paths are wired in Sub-project 1;
// everything else is interfaces + skeletons for sub-project 3.
#include <cstdint>
#include <variant>

namespace mecvr::input {

// 2D stick axis. y+ is forward/up by convention (see docs/INPUT.md).
struct Vector2 {
  float x = 0.0f;
  float y = 0.0f;
};

// Analog scalar/2D or digital payload. monostate = no data this frame.
using ActionValue = std::variant<std::monostate, bool, float, Vector2>;

enum class ActionId {
  // Pose inputs.
  kHeadPose,
  kLeftHandGripPose,
  kLeftHandAimPose,
  kRightHandGripPose,
  kRightHandAimPose,
  // Continuous inputs.
  kMove2D,
  kTurn2D,
  kLook2D,  // Mouse/gamepad fallback look, never the HMD pose.
  kTriggerLeft,
  kTriggerRight,
  kGripLeft,
  kGripRight,
  // Discrete intents.
  kJump,
  kCrouch,
  kSlide,
  kInteract,
  kMelee,
  kHeavyMelee,
  kAbility,
  kSprint,
  kMenu,
  kPause,
  kRecenter,
  // Derived MEC actions: game-derived, never produced by input.
  kVault,
  kWallRun,
  kClimb,
  kSwing,
  kRope,
};

enum class ActionFamily {
  kHead,           // HEAD: always HMD when VR is active.
  kPose,           // POSE: Touch when tracked, never stolen by sticks.
  kLocomotionTurn,  // LOCOMOTION/TURN: stick arbitration (threshold+hysteresis).
  kDigital,        // DIGITAL: presses merge from both devices.
  kMotionGesture,  // MOTION/GESTURE: Touch-only (reserved; no member yet).
  kSystem,         // SYSTEM: recenter/menu accept either route.
};

enum class ActionSource {
  kNone,
  kOpenXr,  // HMD + Touch route.
  kXInput,  // Xbox gamepad route.
  kKeyboardMouse,
  kTest,
};

// Per spec section 12: gameplay code sees values + ownership, never the
// originating device. Pose validity/tracking quality travel in PoseState,
// separately from button state.
struct ActionState {
  ActionValue value;
  ActionSource source = ActionSource::kNone;
  std::uint64_t timestamp = 0;  // Source clock tick.
  bool pressed = false;         // Down edge this frame.
  bool released = false;        // Up edge this frame.
  bool held = false;            // Down level. A held action stays owned by
                                // its original source until release.
};

// Pose validity / tracking quality, carried separately from button state.
struct PoseState {
  bool valid = false;
  float quality = 0.0f;  // 0..1 tracking confidence, meaningful only if valid.
  float position[3] = {0.0f, 0.0f, 0.0f};
  float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

enum class InputMode { kMotion, kHybridAuto, kXboxVr, kCustom };
enum class DominantHand { kRight, kLeft };
enum class TurnMode { kSmooth, kSnap };
enum class MovementReference { kHead, kBody, kLeftHand };

ActionFamily FamilyOf(ActionId id);
bool IsGameDerived(ActionId id);  // True for Vault/WallRun/Climb/Swing/Rope.
const char* ActionName(ActionId id);  // Stable ASCII key for the binding schema.
bool TryParseActionName(const char* name, ActionId* out);

// Single-frame digital edge transition. A held action keeps its original
// source even if a second device also goes down; that is the held-owned-by-
// originator-until-release rule in per-frame form.
inline ActionState NextDigitalState(const ActionState& prev, bool down,
                                    ActionSource source,
                                    std::uint64_t timestamp) {
  ActionState next;
  next.timestamp = timestamp;
  next.value = ActionValue(down);
  next.held = down;
  next.pressed = down && !prev.held;
  next.released = !down && prev.held;
  if (down) {
    next.source = prev.held ? prev.source : source;
  } else {
    next.source = prev.held ? prev.source : ActionSource::kNone;
  }
  return next;
}

}  // namespace mecvr::input
