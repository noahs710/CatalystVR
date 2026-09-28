#include "input/motion_scheme.h"

#include <cstdio>
#include <variant>

namespace {
int failures = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::printf("FAIL: %s\n", message);
    ++failures;
  }
}

}  // namespace

int main() {
  using namespace mecvr::input;
  PoseState raised_left;
  PoseState raised_right;
  raised_left.valid = true;
  raised_right.valid = true;
  raised_left.position[1] = 1.80f;
  raised_right.position[1] = 1.82f;
  Check(HandsRaisedForJump(1.70, raised_left, raised_right),
        "both hands above head produce jump gesture");
  raised_right.position[1] = 1.70f;
  Check(!HandsRaisedForJump(1.70, raised_left, raised_right),
        "one hand below head suppresses jump gesture");
  MotionScheme scheme;
  MotionSample first;
  first.timestamp = 0;
  first.left_hand.valid = true;
  first.right_hand.valid = true;
  first.left_hand.position[2] = 0.0f;
  first.right_hand.position[2] = 0.0f;
  scheme.Update(first);

  MotionSample swing = first;
  swing.timestamp = 100;
  swing.left_hand.position[2] = 0.2f;
  swing.right_hand.position[2] = -0.2f;
  const RawFrame swing_frame = scheme.Update(swing);
  const Vector2 movement =
      std::get<Vector2>(swing_frame.values.at(ActionId::kMove2D));
  Check(movement.y > 0.0f, "alternating arm swing produces movement");

  MotionSample sprint = swing;
  sprint.timestamp = 200;
  sprint.left_hand.position[2] = 0.5f;
  sprint.right_hand.position[2] = -0.5f;
  const RawFrame sprint_frame = scheme.Update(sprint);
  Check(std::get<bool>(sprint_frame.values.at(ActionId::kSprint)),
        "fast alternating swing produces sprint");

  MotionSample stick = sprint;
  stick.timestamp = 300;
  stick.left_stick = {0.0f, 0.8f};
  stick.left_hand.position[2] = 0.51f;
  stick.right_hand.position[2] = -0.51f;
  const RawFrame stick_frame = scheme.Update(stick);
  Check(std::get<Vector2>(stick_frame.values.at(ActionId::kMove2D)).y == 0.8f,
        "active stick wins over arm swing");

  MotionSample punch = stick;
  punch.timestamp = 350;
  punch.left_grip = true;
  punch.left_hand.position[2] = 0.7f;
  const RawFrame punch_frame = scheme.Update(punch);
  Check(std::get<bool>(punch_frame.values.at(ActionId::kMelee)),
        "gripped fast hand motion produces melee intent");
  MotionSample held_punch = punch;
  held_punch.timestamp = 400;
  held_punch.left_hand.position[2] = 0.9f;
  Check(!std::get<bool>(scheme.Update(held_punch).values.at(ActionId::kMelee)),
        "sustained gripped motion does not spam combat pulses");
  MotionSample released_punch = held_punch;
  released_punch.timestamp = 450;
  released_punch.left_grip = false;
  scheme.Update(released_punch);
  MotionSample second_punch = released_punch;
  second_punch.timestamp = 600;
  second_punch.left_grip = true;
  second_punch.left_hand.position[2] = 2.0f;
  Check(std::get<bool>(scheme.Update(second_punch).values.at(ActionId::kMelee)),
        "a released gripped swing can trigger combat again");

  MotionScheme intent_scheme;
  MotionSample rope_start;
  rope_start.timestamp = 0;
  rope_start.head.valid = true;
  rope_start.head.position[2] = 0.0f;
  rope_start.left_hand.valid = true;
  rope_start.left_hand.position[0] = -0.35f;
  rope_start.left_hand.position[2] = -0.70f;
  rope_start.left_grip = true;
  intent_scheme.Update(rope_start);
  MotionSample rope_pull = rope_start;
  rope_pull.timestamp = 100;
  rope_pull.left_hand.position[2] = -0.62f;
  const RawFrame rope_frame = intent_scheme.Update(rope_pull);
  Check(intent_scheme.latestIntent().left.fist,
        "grip closes the mod-owned hand pose");
  Check(intent_scheme.latestIntent().left.mag_rope_held,
        "grip plus pull toward body latches MAG rope intent");
  Check(!std::get<bool>(rope_frame.values.at(ActionId::kMelee)),
        "rope pull does not create a combat pulse");
  MotionSample rope_release = rope_pull;
  rope_release.timestamp = 200;
  rope_release.left_grip = false;
  intent_scheme.Update(rope_release);
  Check(!intent_scheme.latestIntent().left.mag_rope_held,
        "releasing grip releases MAG rope intent");

  if (failures == 0) std::printf("motion_scheme: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
