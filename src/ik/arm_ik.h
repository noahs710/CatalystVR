#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "camera/math.h"

namespace mecvr::ik {

using camera::Quat;
using camera::Vec3;

struct TrackedPose {
  Vec3 position{};
  Quat orientation{};
  bool valid = false;
};

struct ArmRig {
  Vec3 shoulder{};
  Vec3 pole{};
  double upper_arm = 0.30;
  double forearm = 0.27;
  double hand = 0.12;
};

struct ArmTarget {
  TrackedPose hand{};
  Vec3 pole{};
  double weight = 1.0;
};

struct ArmPose {
  TrackedPose shoulder{};
  TrackedPose elbow{};
  TrackedPose wrist{};
  TrackedPose hand{};
  double reach_error = 0.0;
  bool clamped = false;
  bool valid = false;
};

// A bounded FABRIK solve for shoulder -> elbow -> wrist -> hand. The solver
// never allocates, preserves segment lengths, and clamps unreachable targets
// instead of producing NaNs. It is suitable for deterministic replay/tests
// and for a later game-skeleton adapter.
ArmPose SolveArm(const ArmRig& rig, const ArmTarget& target,
                 std::size_t iterations = 12);

struct MotionHand {
  TrackedPose pose{};
  float trigger = 0.0f;
  float grip = 0.0f;
  bool palm_open = false;
};

struct MotionBody {
  TrackedPose head{};
  Vec3 body_origin{};
  Quat body_orientation{};
};

struct ArmFrame {
  ArmPose left{};
  ArmPose right{};
  bool valid = false;
};

class ArmFrameMailbox {
 public:
  void publish(const ArmFrame& frame);
  bool latest(ArmFrame* out) const;

 private:
  mutable std::mutex mutex_;
  ArmFrame frame_{};
  std::uint64_t sequence_ = 0;
};

// Converts tracked controller poses into the mod-owned animation pose. The
// default rig is symmetric and can be replaced by a title-specific adapter.
ArmFrame BuildMotionArmFrame(const MotionBody& body, const MotionHand& left,
                             const MotionHand& right);

}  // namespace mecvr::ik
