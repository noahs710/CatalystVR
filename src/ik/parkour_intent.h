#pragma once

#include "ik/arm_ik.h"

namespace mecvr::ik {

// Shared, deterministic physical-gesture contract for mod-owned parkour
// animation. These intents are deliberately separate from Catalyst-derived
// actions: consumers may render the pose without synthesizing private game
// state or writing engine memory.
struct ParkourIntentInput {
  Vec3 head_position{};
  Vec3 floor_origin{};
  Vec3 velocity{};
  MotionHand left_hand{};
  MotionHand right_hand{};
  bool grounded = true;
};

struct ParkourIntents {
  bool climbing = false;
  bool sliding = false;
  bool vaulting = false;
  bool wall_running = false;
};

ParkourIntents ClassifyParkourIntents(const ParkourIntentInput& input);

}  // namespace mecvr::ik
