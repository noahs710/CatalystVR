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
  punch.left_hand.position[2] = 0.7f;
  const RawFrame punch_frame = scheme.Update(punch);
  Check(std::get<bool>(punch_frame.values.at(ActionId::kMelee)),
        "fast hand motion produces melee intent");

  if (failures == 0) std::printf("motion_scheme: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
