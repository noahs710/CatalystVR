#include <cmath>
#include <iostream>

#include "ik/arm_ik.h"

namespace {

double Distance(mecvr::camera::Vec3 a, mecvr::camera::Vec3 b) {
  const double x = a.x - b.x;
  const double y = a.y - b.y;
  const double z = a.z - b.z;
  return std::sqrt(x * x + y * y + z * z);
}

bool Near(double a, double b, double epsilon = 1e-6) {
  return std::fabs(a - b) <= epsilon;
}

bool Check(bool condition, const char* label) {
  if (!condition) std::cerr << "FAIL: " << label << '\n';
  return condition;
}

}  // namespace

int main() {
  using mecvr::camera::Quat;
  using mecvr::camera::Vec3;
  using mecvr::ik::ArmRig;
  using mecvr::ik::ArmTarget;
  using mecvr::ik::TrackedPose;

  ArmRig rig;
  rig.shoulder = {0.0, 1.4, 0.0};
  rig.pole = {-0.2, 1.2, -0.4};
  rig.upper_arm = 0.30;
  rig.forearm = 0.27;
  rig.hand = 0.12;
  TrackedPose target_pose;
  target_pose.position = {0.20, 1.20, -0.42};
  target_pose.orientation = Quat{};
  target_pose.valid = true;

  const auto pose = mecvr::ik::SolveArm(rig, {target_pose, rig.pole, 1.0});
  bool ok = true;
  ok &= Check(pose.valid, "reachable arm valid");
  ok &= Check(pose.reach_error < 1e-5, "reachable hand target");
  ok &= Check(Near(Distance(pose.shoulder.position, pose.elbow.position),
                   rig.upper_arm, 1e-5),
              "upper-arm length");
  ok &= Check(Near(Distance(pose.elbow.position, pose.wrist.position),
                   rig.forearm, 1e-5),
              "forearm length");
  ok &= Check(Near(Distance(pose.wrist.position, pose.hand.position), rig.hand,
                   1e-5),
              "hand length");

  target_pose.position = {10.0, 1.4, 0.0};
  const auto clamped = mecvr::ik::SolveArm(rig, {target_pose, rig.pole, 1.0});
  ok &= Check(clamped.valid && clamped.clamped, "unreachable target clamped");
  ok &= Check(Near(Distance(clamped.shoulder.position, clamped.hand.position),
                   rig.upper_arm + rig.forearm + rig.hand, 1e-5),
              "clamped endpoint stable");

  mecvr::ik::MotionBody body;
  body.body_origin = {0.0, 0.0, 0.0};
  mecvr::ik::MotionHand left;
  left.pose.position = {-0.35, 1.2, -0.35};
  left.pose.valid = true;
  mecvr::ik::MotionHand right;
  right.pose.position = {0.35, 1.2, -0.35};
  right.pose.valid = true;
  const auto frame = mecvr::ik::BuildMotionArmFrame(body, left, right);
  ok &= Check(frame.valid && frame.left.valid && frame.right.valid,
              "two-arm motion frame");

  std::cout << (ok ? "ARM_IK_TEST: PASS\n" : "ARM_IK_TEST: FAIL\n");
  return ok ? 0 : 1;
}
