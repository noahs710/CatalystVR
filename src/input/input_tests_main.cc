// T4 input unit tests: plain assert-style exe main in the smoke-test style.
// CTest wiring is deferred to integration (T1 owns tests/CMakeLists.txt);
// see docs/INPUT.md. Compile with cl.exe directly (command in INPUT.md).
#include <cstdio>

#include "config/input_bindings.h"
#include "input/arbitration.h"
#include "input/openxr_device.h"
#include "input/xinput_device.h"

namespace {

int failures = 0;
void Check(bool ok, const char* name) {
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", name);
  } else {
    std::printf("ok: %s\n", name);
  }
}

using mecvr::input::ActionFamily;
using mecvr::input::ActionId;
using mecvr::input::ActionSource;
using mecvr::input::ActionState;
using mecvr::input::DigitalArbiter;
using mecvr::input::NextDigitalState;
using mecvr::input::StickArbiter;
using mecvr::input::StickArbiterConfig;
using mecvr::input::StickOwner;
using mecvr::input::Vector2;

Vector2 Stick(float x, float y) {
  Vector2 v;
  v.x = x;
  v.y = y;
  return v;
}

void TestActionStateTransitions() {
  ActionState idle;
  ActionState down =
      NextDigitalState(idle, true, ActionSource::kOpenXr, 100);
  Check(down.pressed && down.held && !down.released, "digital press edge");
  Check(down.source == ActionSource::kOpenXr, "press attributes source");
  Check(down.timestamp == 100, "timestamp carried");

  // Second device also goes down: held stays owned by the originator.
  ActionState both =
      NextDigitalState(down, true, ActionSource::kXInput, 101);
  Check(both.held && !both.pressed && !both.released, "no re-press while held");
  Check(both.source == ActionSource::kOpenXr, "held keeps originator");

  ActionState released = NextDigitalState(both, false, ActionSource::kXInput, 102);
  Check(released.released && !released.held, "release edge");
  Check(released.source == ActionSource::kOpenXr, "release attributes owner");

  ActionState quiet = NextDigitalState(released, false, ActionSource::kNone, 103);
  Check(!quiet.pressed && !quiet.released && !quiet.held, "idle settles");
  Check(quiet.source == ActionSource::kNone, "idle has no owner");
}

void TestStickOwnershipAndHysteresis() {
  StickArbiterConfig config;
  config.activation_threshold = 0.35f;
  config.deadzone = 0.15f;
  config.release_hysteresis_frames = 3;
  StickArbiter arbiter(config);

  Check(arbiter.owner() == StickOwner::kNone, "stick starts unowned");
  // Sub-deadzone noise never claims ownership.
  Check(arbiter.Update(Stick(0.05f, 0.0f), Stick(0.0f, -0.08f)) ==
            StickOwner::kNone,
        "noise rejected");
  // Touch exceeds the activation threshold and claims ownership.
  Check(arbiter.Update(Stick(0.0f, 0.6f), Stick(0.0f, 0.0f)) ==
            StickOwner::kTouch,
        "touch claims past threshold");
  // Xbox goes hard while Touch is owned: no steal mid-hold.
  Check(arbiter.Update(Stick(0.0f, 0.6f), Stick(0.9f, 0.0f)) ==
            StickOwner::kTouch,
        "challenger cannot steal active owner");
  // Touch drops to neutral: hysteresis keeps ownership briefly.
  Check(arbiter.Update(Stick(0.0f, 0.0f), Stick(0.9f, 0.0f)) ==
            StickOwner::kTouch,
        "hysteresis holds through neutral");
  Check(arbiter.Update(Stick(0.0f, 0.0f), Stick(0.9f, 0.0f)) ==
            StickOwner::kTouch,
        "hysteresis holds, challenger waits");
  // After hysteresis elapses the active challenger claims fresh.
  bool handed_off = false;
  for (int i = 0; i < 4; ++i) {
    if (arbiter.Update(Stick(0.0f, 0.0f), Stick(0.9f, 0.0f)) ==
        StickOwner::kXbox) {
      handed_off = true;
    }
  }
  Check(handed_off, "ownership hands off after hysteresis");
  // Owner stick back to neutral with no challenger: returns to unowned.
  for (int i = 0; i < 6; ++i) {
    arbiter.Update(Stick(0.0f, 0.0f), Stick(0.0f, 0.0f));
  }
  Check(arbiter.owner() == StickOwner::kNone, "neutral returns to unowned");
  // Deadzone-sized deflection after release does not reclaim.
  Check(arbiter.Update(Stick(0.0f, 0.0f), Stick(0.2f, 0.0f)) ==
            StickOwner::kNone,
        "deadzone deflection does not claim");
}

void TestHeldSourceOwnership() {
  DigitalArbiter jump;
  const ActionState& press = jump.Update(true, false, 10);
  Check(press.pressed && press.held, "touch press registers");
  Check(press.source == ActionSource::kOpenXr, "touch owns press");
  // Xbox joins mid-hold: attribution stays with the originator.
  const ActionState& joined = jump.Update(true, true, 11);
  Check(joined.held && !joined.pressed, "join is not a new press");
  Check(joined.source == ActionSource::kOpenXr, "originator keeps hold");
  // Originator releases while Xbox still holds: merged hold continues,
  // still attributed to the originator until full release.
  const ActionState& carried = jump.Update(false, true, 12);
  Check(carried.held && !carried.released, "merged hold survives originator up");
  Check(carried.source == ActionSource::kOpenXr, "attribution until release");
  // Full release: single release edge attributed to the originator.
  const ActionState& up = jump.Update(false, false, 13);
  Check(up.released && !up.held, "full release edges once");
  Check(up.source == ActionSource::kOpenXr, "release attributes originator");
  const ActionState& idle = jump.Update(false, false, 14);
  Check(!idle.released && idle.source == ActionSource::kNone, "idle clears");

  // Xbox-only press attributes Xbox (SYSTEM either-route behaves the same).
  DigitalArbiter menu;
  const ActionState& xbox_press = menu.Update(false, true, 20);
  Check(xbox_press.pressed && xbox_press.source == ActionSource::kXInput,
        "xbox-only press attributes xbox");
}

void TestFamilyIsolation() {
  StickArbiterConfig config;
  StickArbiter move(config);
  StickArbiter turn(config);
  // Xbox owns locomotion while Touch owns turning: independent families.
  move.Update(Stick(0.0f, 0.0f), Stick(0.0f, 0.8f));
  turn.Update(Stick(0.7f, 0.0f), Stick(0.0f, 0.0f));
  Check(move.owner() == StickOwner::kXbox && turn.owner() == StickOwner::kTouch,
        "families own independently");
  move.Reset();
  Check(move.owner() == StickOwner::kNone &&
            turn.owner() == StickOwner::kTouch,
        "reset is per family");

  // HEAD is always HMD; POSE is Touch when tracked, never Xbox.
  Check(mecvr::input::HeadSource() == ActionSource::kOpenXr,
        "head always hmd");
  Check(mecvr::input::PoseOwner(true) == ActionSource::kOpenXr,
        "pose touch when tracked");
  Check(mecvr::input::PoseOwner(false) == ActionSource::kNone,
        "pose never xbox");

  // Taxonomy guards: derived MEC actions are never input-produced.
  Check(mecvr::input::IsGameDerived(ActionId::kVault) &&
            mecvr::input::IsGameDerived(ActionId::kWallRun) &&
            mecvr::input::IsGameDerived(ActionId::kClimb) &&
            mecvr::input::IsGameDerived(ActionId::kSwing) &&
            mecvr::input::IsGameDerived(ActionId::kRope),
        "derived actions flagged");
  Check(!mecvr::input::IsGameDerived(ActionId::kJump) &&
            !mecvr::input::IsGameDerived(ActionId::kMove2D),
        "intents are not derived");
  Check(mecvr::input::FamilyOf(ActionId::kHeadPose) == ActionFamily::kHead,
        "head family");
  Check(mecvr::input::FamilyOf(ActionId::kLeftHandAimPose) ==
            ActionFamily::kPose,
        "pose family");
  Check(mecvr::input::FamilyOf(ActionId::kTurn2D) ==
            ActionFamily::kLocomotionTurn,
        "locomotion family");
  Check(mecvr::input::FamilyOf(ActionId::kMenu) == ActionFamily::kSystem,
        "system family");
  ActionId parsed = ActionId::kJump;
  Check(mecvr::input::TryParseActionName("Recenter", &parsed) &&
            parsed == ActionId::kRecenter,
        "action name round-trip");
  Check(!mecvr::input::TryParseActionName("Nope", &parsed),
        "unknown action rejected");
}

void TestSchemaLoad() {
  mecvr::config::Registry registry;
  Check(mecvr::config::RegisterInputBindings(registry), "register section");
  Check(!mecvr::config::RegisterInputBindings(registry),
        "duplicate section rejected");

  mecvr::config::InputBindings defaults =
      mecvr::config::DefaultInputBindings();
  Check(!defaults.actions.empty(), "defaults non-empty");
  Check(defaults.actions.count("Vault") == 0, "derived actions unbound");
  Check(mecvr::config::SaveInputBindings(registry, defaults),
        "bindings save to registry");

  mecvr::config::InputBindings loaded;
  Check(mecvr::config::LoadInputBindings(registry, &loaded),
        "bindings load from registry");
  Check(loaded.actions == defaults.actions, "bindings map round-trips");
  Check(loaded.mode == defaults.mode &&
            loaded.dominant_hand == defaults.dominant_hand &&
            loaded.turn_mode == defaults.turn_mode &&
            loaded.movement_reference == defaults.movement_reference,
        "preference enums round-trip");

  // Malformed binding entries are skipped; bad enums fail closed.
  Check(registry.set("input_bindings", "Jump", "touch:a_button;broken;xbox:"),
        "store malformed list");
  Check(mecvr::config::LoadInputBindings(registry, &loaded),
        "malformed entries do not fail load");
  Check(loaded.actions["Jump"].size() == 1 &&
            loaded.actions["Jump"][0].route == "touch",
        "malformed entries skipped");
  Check(registry.set("input_bindings", "mode", "warp_drive"),
        "store bad enum");
  Check(!mecvr::config::LoadInputBindings(registry, &loaded),
        "bad enum fails closed");
  Check(!mecvr::config::LoadInputBindings(registry, nullptr),
        "null out rejected");
}

void TestSkeletonDevices() {
  mecvr::input::OpenXrInputDevice xr;
  mecvr::input::XInputDevice pad;
  Check(xr.source() == ActionSource::kOpenXr, "xr source");
  Check(pad.source() == ActionSource::kXInput, "xinput source");
  Check(!xr.connected() && !pad.connected(), "skeletons start offline");

  mecvr::input::RawFrame xr_frame = xr.Poll(42);
  Check(xr_frame.timestamp == 42 && xr_frame.values.empty(),
        "xr canned frame neutral");
  Check(xr.RequestRecenter(), "recenter route meaningful");

  xr.set_connected(true);
  mecvr::input::RawFrame test;
  test.values[mecvr::input::ActionId::kJump] =
      mecvr::input::ActionValue(true);
  xr.SetTestFrame(test);
  mecvr::input::RawFrame injected = xr.Poll(43);
  Check(injected.connected && injected.values.count(ActionId::kJump) == 1,
        "test frame injection");
  xr.ClearTestFrame();
  Check(xr.Poll(44).values.empty(), "test frame clears");
  Check(pad.RequestRecenter(), "xinput either-route recenter");
}

}  // namespace

int main() {
  TestActionStateTransitions();
  TestStickOwnershipAndHysteresis();
  TestHeldSourceOwnership();
  TestFamilyIsolation();
  TestSchemaLoad();
  TestSkeletonDevices();
  if (failures == 0) std::printf("input: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
