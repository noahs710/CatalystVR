#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>

#include "ik/full_body_ik.h"
#include "ik/height_calibration.h"

namespace {

using mecvr::camera::Vec3;
using mecvr::ik::BodyAnimationState;
using mecvr::ik::BodyJoint;
using mecvr::ik::FullBodyAnimator;
using mecvr::ik::FullBodyInput;
using mecvr::ik::MotionClip;

double Distance(Vec3 a, Vec3 b) {
  const double x = a.x - b.x;
  const double y = a.y - b.y;
  const double z = a.z - b.z;
  return std::sqrt(x * x + y * y + z * z);
}

bool Near(double a, double b, double epsilon = 1e-5) {
  return std::fabs(a - b) <= epsilon;
}

bool Check(bool condition, const char* label) {
  if (!condition) std::cerr << "FAIL: " << label << '\n';
  return condition;
}

FullBodyInput StandingInput() {
  FullBodyInput input;
  input.sequence = 7;
  input.sample_time_ns = 123456;
  input.head.valid = true;
  input.head.position = {0.0, 1.70, 0.0};
  input.head.orientation = {};
  input.left_hand.pose.valid = true;
  input.left_hand.pose.position = {-0.38, 1.25, -0.30};
  input.left_hand.trigger = 0.35f;
  input.left_hand.grip = 0.70f;
  input.right_hand.pose.valid = true;
  input.right_hand.pose.position = {0.38, 1.25, -0.30};
  input.right_hand.trigger = 0.85f;
  input.right_hand.grip = 0.15f;
  input.floor_origin = {0.0, 0.0, 0.0};
  input.delta_seconds = 1.0 / 90.0;
  return input;
}

}  // namespace

int main() {
  bool ok = true;
  mecvr::ik::HeightCalibrator height;
  double floor_y = 0.0;
  ok &= Check(height.update(1.70, &floor_y) && Near(floor_y, 0.0),
              "height calibrates standing floor");
  ok &= Check(height.update(1.35, &floor_y) && Near(floor_y, 0.0),
              "height remains stable while crouching");
  ok &= Check(mecvr::ik::IsPhysicallyCrouched(1.30, floor_y),
              "physical crouch threshold activates");
  ok &= Check(!mecvr::ik::IsPhysicallyCrouched(1.45, floor_y),
              "physical crouch threshold releases");
  height.reset();
  ok &= Check(height.update(1.80, &floor_y) && Near(floor_y, 0.10),
              "height recalibrates after recenter");
  FullBodyAnimator animator;
  const FullBodyInput standing = StandingInput();
  const auto idle = animator.update(standing);
  ok &= Check(idle.valid, "standing pose valid");
  ok &= Check(idle.state == BodyAnimationState::kIdle, "idle selected");
  ok &= Check(idle.sequence == standing.sequence, "sequence preserved");
  ok &= Check(Near(idle.trigger_values[0], 0.35), "left trigger preserved");
  ok &= Check(Near(idle.trigger_values[1], 0.85), "right trigger preserved");
  ok &= Check(Near(idle.grip_values[0], 0.70), "left grip preserved");
  ok &= Check(Near(idle.grip_values[1], 0.15), "right grip preserved");
  ok &= Check(Near(Distance(idle.joint(BodyJoint::kLeftHip).position,
                            idle.joint(BodyJoint::kLeftKnee).position),
                   0.43),
              "left thigh length");
  ok &= Check(Near(Distance(idle.joint(BodyJoint::kLeftKnee).position,
                            idle.joint(BodyJoint::kLeftAnkle).position),
                   0.43),
              "left shin length");
  ok &= Check(Near(idle.joint(BodyJoint::kLeftFoot).position.y,
                   idle.joint(BodyJoint::kRightFoot).position.y),
              "idle feet level");

  FullBodyInput fired = standing;
  fired.left_hand.trigger = 1.0f;
  fired.right_hand.trigger = 0.0f;
  const auto shot = animator.update(fired);
  ok &= Check(shot.shot_pulses[0] && !shot.shot_pulses[1],
              "trigger edge creates left shot pulse");
  const auto held = animator.update(fired);
  ok &= Check(!held.shot_pulses[0], "held trigger does not retrigger");

  FullBodyAnimator recoil_animator;
  FullBodyInput neutral = standing;
  neutral.left_hand.trigger = 0.0f;
  neutral.right_hand.trigger = 0.0f;
  const auto neutral_frame = recoil_animator.update(neutral);
  FullBodyInput recoil_input = neutral;
  recoil_input.left_hand.trigger = 1.0f;
  const auto recoil_frame = recoil_animator.update(recoil_input);
  ok &= Check(
      recoil_frame.joint(BodyJoint::kLeftHand).position.z >
          neutral_frame.joint(BodyJoint::kLeftHand).position.z + 0.001,
      "shot edge adds authored hand recoil");
  recoil_input.left_hand.trigger = 0.0f;
  const auto recovered_frame = recoil_animator.update(recoil_input);
  ok &= Check(recovered_frame.joint(BodyJoint::kLeftHand).position.z <
                  recoil_frame.joint(BodyJoint::kLeftHand).position.z,
              "hand recoil recovers deterministically");

  FullBodyInput moving = standing;
  moving.velocity = {0.0, 0.0, -2.0};
  const auto gait_a = animator.update(moving);
  const auto gait_b = animator.update(moving);
  ok &= Check(gait_a.state == BodyAnimationState::kLocomotion,
              "locomotion selected");
  ok &= Check(gait_b.gait_phase > gait_a.gait_phase, "gait advances");
  const double left_y = gait_b.joint(BodyJoint::kLeftAnkle).position.y;
  const double right_y = gait_b.joint(BodyJoint::kRightAnkle).position.y;
  ok &= Check(!Near(left_y, right_y, 1e-4), "alternating foot swing");

  FullBodyInput crouched = standing;
  crouched.head.position.y = 1.15;
  const auto crouch = animator.update(crouched);
  ok &= Check(crouch.state == BodyAnimationState::kCrouch, "crouch selected");
  ok &= Check(crouch.joint(BodyJoint::kPelvis).position.y <
                  idle.joint(BodyJoint::kPelvis).position.y,
              "crouch lowers pelvis");

  FullBodyInput airborne = standing;
  airborne.grounded = false;
  airborne.velocity.y = 2.0;
  const auto air = animator.update(airborne);
  ok &= Check(air.state == BodyAnimationState::kAirborne,
              "airborne selected");

  FullBodyInput vaulting = standing;
  vaulting.left_hand.grip = 1.0f;
  vaulting.right_hand.grip = 1.0f;
  vaulting.left_hand.pose.position.y = 1.55;
  vaulting.right_hand.pose.position.y = 1.55;
  vaulting.velocity.y = -1.0;
  vaulting.vaulting = true;
  const auto vault = animator.update(vaulting);
  ok &= Check(vault.state == BodyAnimationState::kVault,
              "vault gesture selects authored vault state");

  FullBodyInput sliding = standing;
  sliding.head.position.y = 1.15;
  sliding.velocity = {0.0, 0.0, -2.0};
  sliding.sliding = true;
  const auto slide = animator.update(sliding);
  ok &= Check(slide.state == BodyAnimationState::kSlide,
              "slide intent selects authored slide state");

  FullBodyInput wall_running = standing;
  wall_running.velocity = {2.0, 0.0, 0.0};
  wall_running.wall_running = true;
  const auto wall_run = animator.update(wall_running);
  ok &= Check(wall_run.state == BodyAnimationState::kWallRun,
              "wall-run intent selects authored wall-run state");

  FullBodyAnimator replay_a;
  FullBodyAnimator replay_b;
  const auto deterministic_a = replay_a.update(moving);
  const auto deterministic_b = replay_b.update(moving);
  ok &= Check(Near(deterministic_a.gait_phase, deterministic_b.gait_phase),
              "deterministic phase");
  ok &= Check(Near(Distance(deterministic_a.joint(BodyJoint::kLeftFoot).position,
                            deterministic_b.joint(BodyJoint::kLeftFoot).position),
                   0.0),
              "deterministic joints");

  FullBodyInput tracked = standing;
  tracked.has_tracked_body = true;
  tracked.tracked_joints[static_cast<std::size_t>(BodyJoint::kChest)].valid =
      true;
  tracked.tracked_joints[static_cast<std::size_t>(BodyJoint::kChest)].position =
      {0.12, 1.47, -0.22};
  const auto procedural_baseline = animator.update(standing);
  const auto authoritative = animator.update(tracked);
  ok &= Check((authoritative.source_flags & mecvr::ik::kSourceFullBody) != 0,
              "full-body provider source flagged");
  ok &= Check(Near(authoritative.joint(BodyJoint::kChest).position.x, 0.12),
              "valid tracked joint overrides procedural pose");
  ok &= Check(Near(authoritative.joint(BodyJoint::kLeftKnee).position.x,
                   procedural_baseline.joint(BodyJoint::kLeftKnee).position.x),
              "missing tracked joint retains procedural pose");

  FullBodyInput malformed_body = standing;
  malformed_body.has_tracked_body = true;
  malformed_body.tracked_joints[static_cast<std::size_t>(BodyJoint::kChest)] =
      tracked.tracked_joints[static_cast<std::size_t>(BodyJoint::kChest)];
  malformed_body.tracked_joints[static_cast<std::size_t>(BodyJoint::kChest)]
      .position.x = std::numeric_limits<double>::quiet_NaN();
  const auto safe_body = animator.update(malformed_body);
  ok &= Check((safe_body.source_flags & mecvr::ik::kSourceFullBody) == 0,
              "malformed tracked joint is rejected");
  ok &= Check(std::isfinite(safe_body.joint(BodyJoint::kChest).position.x),
              "malformed tracked joint cannot poison pose");

  FullBodyInput malformed_head = standing;
  malformed_head.head.position.y =
      std::numeric_limits<double>::infinity();
  const auto invalid_head = animator.update(malformed_head);
  ok &= Check(!invalid_head.valid, "malformed head invalidates pose");

  FullBodyInput zero_quaternion = standing;
  zero_quaternion.head.orientation = {0.0, 0.0, 0.0, 0.0};
  const auto invalid_quaternion = animator.update(zero_quaternion);
  ok &= Check(!invalid_quaternion.valid,
              "zero quaternion invalidates pose");

  FullBodyInput malformed_motion = standing;
  malformed_motion.velocity.x = std::numeric_limits<double>::quiet_NaN();
  malformed_motion.floor_origin.y =
      std::numeric_limits<double>::infinity();
  const auto sanitized_motion = animator.update(malformed_motion);
  ok &= Check(sanitized_motion.valid,
              "malformed motion falls back safely");
  ok &= Check(std::isfinite(
                  sanitized_motion.joint(BodyJoint::kPelvis).position.x),
              "sanitized pelvis remains finite");

  mecvr::ik::BodyPoseMailbox mailbox;
  mailbox.publish(idle);
  mecvr::ik::HumanoidPoseFrame copied;
  ok &= Check(mailbox.latest(&copied) && copied.sequence == idle.sequence,
              "body mailbox latest");
  FullBodyInput lost = standing;
  lost.head.valid = false;
  const auto invalid = animator.update(lost);
  ok &= Check(!invalid.valid, "tracking loss invalidates new pose");
  mailbox.publish(invalid);
  ok &= Check(mailbox.latest(&copied) && copied.sequence == idle.sequence,
              "invalid pose does not replace valid mailbox frame");

  MotionClip clip(4);
  auto clip_end = idle;
  clip_end.sequence = idle.sequence + 1;
  clip_end.sample_time_ns = idle.sample_time_ns + 1000000000;
  clip_end.joints[static_cast<std::size_t>(BodyJoint::kPelvis)].position.x +=
      1.0;
  ok &= Check(clip.append(idle) && clip.append(clip_end) && clip.size() == 2,
              "motion clip records bounded solved poses");
  mecvr::ik::HumanoidPoseFrame halfway;
  ok &= Check(clip.sample(0.5, &halfway), "motion clip interpolates");
  ok &= Check(Near(halfway.joint(BodyJoint::kPelvis).position.x, 0.5),
              "motion clip interpolates joint position");
  const char* clip_path = "mecvr_motion_clip_test.bin";
  ok &= Check(clip.save(clip_path), "motion clip saves");
  MotionClip loaded;
  ok &= Check(MotionClip::load(clip_path, &loaded) && loaded.size() == 2,
              "motion clip loads");
  std::remove(clip_path);
  MotionClip capped(1);
  ok &= Check(capped.append(idle) && !capped.append(clip_end),
              "motion clip cap rejects overflow");

  std::cout << (ok ? "FULL_BODY_IK_TEST: PASS\n"
                   : "FULL_BODY_IK_TEST: FAIL\n");
  return ok ? 0 : 1;
}
