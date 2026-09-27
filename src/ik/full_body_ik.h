#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "ik/arm_ik.h"

namespace mecvr::ik {

enum class BodyJoint : std::size_t {
  kRoot,
  kPelvis,
  kChest,
  kNeck,
  kHead,
  kLeftShoulder,
  kLeftElbow,
  kLeftWrist,
  kLeftHand,
  kRightShoulder,
  kRightElbow,
  kRightWrist,
  kRightHand,
  kLeftHip,
  kLeftKnee,
  kLeftAnkle,
  kLeftFoot,
  kRightHip,
  kRightKnee,
  kRightAnkle,
  kRightFoot,
  kCount,
};

enum class BodyAnimationState : std::uint8_t {
  kIdle,
  kLocomotion,
  kCrouch,
  kAirborne,
  kClimb,
  kSlide,
  kVault,
  kWallRun,
};

enum BodySourceFlags : std::uint32_t {
  kSourceHead = 1u << 0,
  kSourceLeftHand = 1u << 1,
  kSourceRightHand = 1u << 2,
  kSourceGrounded = 1u << 3,
  kSourceFullBody = 1u << 4,
};

struct BodyCalibration {
  double standing_height = 1.70;
  double eye_to_pelvis = 0.78;
  double shoulder_width = 0.44;
  double hip_width = 0.32;
  double upper_arm = 0.30;
  double forearm = 0.27;
  double hand = 0.12;
  double thigh = 0.43;
  double shin = 0.43;
  double foot = 0.22;
};

struct FullBodyInput {
  std::uint64_t sequence = 0;
  std::int64_t sample_time_ns = 0;
  TrackedPose head{};
  MotionHand left_hand{};
  MotionHand right_hand{};
  Vec3 floor_origin{};
  Vec3 velocity{};
  double delta_seconds = 0.0;
  bool grounded = true;
  bool climbing = false;
  bool sliding = false;
  bool vaulting = false;
  bool wall_running = false;
  // Optional authoritative full-body tracking. Invalid entries retain the
  // procedural solver result, allowing partial runtimes to degrade cleanly.
  std::array<TrackedPose, static_cast<std::size_t>(BodyJoint::kCount)>
      tracked_joints{};
  bool has_tracked_body = false;
};

struct HumanoidPoseFrame {
  std::uint64_t sequence = 0;
  std::int64_t sample_time_ns = 0;
  std::array<TrackedPose, static_cast<std::size_t>(BodyJoint::kCount)> joints{};
  BodyAnimationState state = BodyAnimationState::kIdle;
  std::uint32_t source_flags = 0;
  double gait_phase = 0.0;
  // 0 = left, 1 = right. These values are part of the mod-owned animation
  // contract and let a future game skeleton adapter blend finger poses.
  std::array<float, 2> trigger_values{};
  std::array<float, 2> grip_values{};
  std::array<bool, 2> shot_pulses{};
  bool valid = false;

  const TrackedPose& joint(BodyJoint value) const {
    return joints[static_cast<std::size_t>(value)];
  }
};

class FullBodyAnimator {
 public:
  explicit FullBodyAnimator(BodyCalibration calibration = {});
  HumanoidPoseFrame update(const FullBodyInput& input);
  void reset();

 private:
  BodyCalibration calibration_{};
  double gait_phase_ = 0.0;
  std::array<float, 2> previous_triggers_{};
  std::array<double, 2> recoil_{};
};

class BodyPoseMailbox {
 public:
  void publish(const HumanoidPoseFrame& frame);
  bool latest(HumanoidPoseFrame* out) const;

 private:
  mutable std::mutex mutex_;
  HumanoidPoseFrame frame_{};
  std::uint64_t sequence_ = 0;
};

// Bounded mod-owned animation clip. It stores solved IK output rather than
// device-specific input, so a recording is deterministic across OpenXR
// runtimes and can be replayed by a future native skeleton adapter.
class MotionClip {
 public:
  explicit MotionClip(std::size_t max_frames = 1800);

  bool append(const HumanoidPoseFrame& frame);
  void clear();
  std::size_t size() const { return frames_.size(); }
  bool empty() const { return frames_.empty(); }
  const std::vector<HumanoidPoseFrame>& frames() const { return frames_; }
  double durationSeconds() const;

  // Samples by elapsed seconds from the first recorded frame. Position and
  // orientation are interpolated; animation state and shot pulses are held
  // from the preceding keyframe.
  bool sample(double elapsed_seconds, HumanoidPoseFrame* out) const;

  bool save(const std::string& path) const;
  static bool load(const std::string& path, MotionClip* out);

 private:
  std::size_t max_frames_ = 1800;
  std::vector<HumanoidPoseFrame> frames_;
};

}  // namespace mecvr::ik
