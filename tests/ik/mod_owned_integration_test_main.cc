#include <cmath>
#include <iostream>

#include "ik/full_body_ik.h"
#include "input/motion_scheme.h"
#include "render/body_overlay_geometry.h"

namespace {

bool Check(bool value, const char* label) {
  if (!value) std::cerr << "FAIL: " << label << '\n';
  return value;
}

mecvr::ik::FullBodyInput Input(std::uint64_t sequence, float left_trigger,
                               float right_trigger, double head_y = 1.70) {
  mecvr::ik::FullBodyInput input;
  input.sequence = sequence;
  input.sample_time_ns = static_cast<std::int64_t>(sequence) * 11111111;
  input.head.valid = true;
  input.head.position = {0.0, head_y, 0.0};
  input.left_hand.pose.valid = true;
  input.left_hand.pose.position = {-0.38, 1.25, -0.30};
  input.left_hand.trigger = left_trigger;
  input.left_hand.grip = 0.80f;
  input.right_hand.pose.valid = true;
  input.right_hand.pose.position = {0.38, 1.25, -0.30};
  input.right_hand.trigger = right_trigger;
  input.right_hand.grip = 0.80f;
  input.floor_origin = {0.0, 0.0, 0.0};
  input.delta_seconds = 1.0 / 90.0;
  return input;
}

}  // namespace

int main() {
  bool ok = true;
  mecvr::ik::FullBodyAnimator animator;
  const auto idle = animator.update(Input(1, 0.0f, 0.0f));
  const auto fired = animator.update(Input(2, 1.0f, 0.0f));
  const auto crouched = animator.update(Input(3, 0.0f, 0.0f, 1.15));
  ok &= Check(idle.valid && fired.valid && crouched.valid,
              "synthetic full-body frames are valid");
  ok &= Check(fired.shot_pulses[0], "synthetic trigger produces shot pulse");
  ok &= Check(fired.joint(mecvr::ik::BodyJoint::kLeftHand).position.z >
                  idle.joint(mecvr::ik::BodyJoint::kLeftHand).position.z,
              "synthetic shot produces recoil in solved hand");
  ok &= Check(crouched.state == mecvr::ik::BodyAnimationState::kCrouch,
              "synthetic head lowering produces crouch state");

  mecvr::ik::MotionClip clip(8);
  ok &= Check(clip.append(idle) && clip.append(fired) && clip.append(crouched),
              "solved frames record into bounded motion clip");
  mecvr::ik::HumanoidPoseFrame replayed;
  ok &= Check(clip.sample(0.011111, &replayed),
              "recorded motion clip samples without HMD");
  ok &= Check(replayed.valid, "sampled motion frame remains valid");

  mecvr::render::BodyOverlayVertex vertices[512]{};
  const std::size_t count = mecvr::render::BuildBodyOverlayGeometry(
      replayed, vertices, 512, -0.032f);
  ok &= Check(count > 0 && count % 3 == 0,
              "sampled pose produces stereo overlay triangles");
  for (std::size_t i = 0; i < count; ++i) {
    ok &= Check(std::isfinite(vertices[i].x) && std::isfinite(vertices[i].y) &&
                    std::isfinite(vertices[i].depth) && vertices[i].depth >= 0.0f &&
                    vertices[i].depth <= 1.0f,
                "stereo overlay vertices remain finite and normalized");
  }

  mecvr::input::PoseState left;
  mecvr::input::PoseState right;
  left.valid = right.valid = true;
  left.position[1] = 1.82f;
  right.position[1] = 1.84f;
  ok &= Check(mecvr::input::HandsRaisedForJump(1.70, left, right),
              "synthetic raised-hands jump intent survives integration path");
  std::cout << (ok ? "MOD_OWNED_INTEGRATION_TEST: PASS\n"
                   : "MOD_OWNED_INTEGRATION_TEST: FAIL\n");
  return ok ? 0 : 1;
}
