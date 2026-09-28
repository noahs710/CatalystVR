#include "ik/parkour_intent.h"

#include <cmath>

namespace mecvr::ik {

ParkourIntents ClassifyParkourIntents(const ParkourIntentInput& input) {
  ParkourIntents out;
  const bool left_valid = input.left_hand.pose.valid;
  const bool right_valid = input.right_hand.pose.valid;
  const bool both_squeezed = left_valid && right_valid &&
                              input.left_hand.grip > 0.65f &&
                              input.right_hand.grip > 0.65f;
  const bool both_hands_high =
      left_valid && right_valid &&
      input.left_hand.pose.position.y > input.head_position.y - 0.25 &&
      input.right_hand.pose.position.y > input.head_position.y - 0.25;
  const double horizontal_speed =
      std::sqrt(input.velocity.x * input.velocity.x +
                input.velocity.z * input.velocity.z);

  // Climbing is deliberately not synthesized from grip. Catalyst owns ledge
  // autograb and its jump-held transition; this classifier only supplies
  // mod-owned animation hints for vault/slide/wall-run presentation.
  out.climbing = false;
  out.vaulting = both_hands_high && both_squeezed &&
                input.velocity.y < -0.75;
  out.sliding = input.grounded && horizontal_speed > 1.0 &&
                input.head_position.y - input.floor_origin.y < 1.30;

  const bool one_hand_high =
      (left_valid && input.left_hand.pose.position.y > input.head_position.y - 0.20 &&
       input.left_hand.grip > 0.65f) ||
      (right_valid &&
       input.right_hand.pose.position.y > input.head_position.y - 0.20 &&
       input.right_hand.grip > 0.65f);
  out.wall_running = !out.vaulting && !out.sliding &&
                     one_hand_high && horizontal_speed > 1.35 &&
                     std::fabs(input.velocity.y) < 0.45;
  return out;
}

}  // namespace mecvr::ik
