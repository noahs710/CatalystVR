#include "ik/parkour_intent.h"

#include <cassert>

namespace {

mecvr::ik::MotionHand Hand(double y, float grip) {
  mecvr::ik::MotionHand hand;
  hand.pose.valid = true;
  hand.pose.position = {0.0, y, 0.0};
  hand.grip = grip;
  return hand;
}

}  // namespace

int main() {
  using mecvr::ik::ParkourIntentInput;
  ParkourIntentInput input;
  input.head_position = {0.0, 1.70, 0.0};
  input.floor_origin = {0.0, 0.0, 0.0};
  input.left_hand = Hand(1.70, 0.9f);
  input.right_hand = Hand(1.70, 0.9f);
  input.velocity = {0.0, -1.0, 0.0};
  auto intents = mecvr::ik::ClassifyParkourIntents(input);
  assert(intents.vaulting);
  assert(!intents.wall_running);

  input.velocity = {2.0, 0.0, 0.0};
  input.head_position.y = 1.20;
  input.left_hand = Hand(1.60, 0.9f);
  input.right_hand = Hand(1.50, 0.0f);
  intents = mecvr::ik::ClassifyParkourIntents(input);
  assert(intents.sliding);
  assert(intents.wall_running);

  input.left_hand = Hand(1.70, 0.9f);
  input.right_hand = Hand(1.70, 0.9f);
  intents = mecvr::ik::ClassifyParkourIntents(input);
  assert(!intents.climbing);
  assert(!intents.sliding);
  assert(!intents.wall_running);
  return 0;
}
