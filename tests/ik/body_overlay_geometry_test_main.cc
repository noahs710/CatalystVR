#include <algorithm>
#include <cmath>
#include <iostream>

#include "ik/full_body_ik.h"
#include "render/body_overlay_geometry.h"

namespace {

bool Check(bool value, const char* label) {
  if (!value) std::cerr << "FAIL: " << label << '\n';
  return value;
}

mecvr::ik::HumanoidPoseFrame Pose(float left_trigger, float right_trigger,
                                  bool rotated = false, float left_grip = 0.25f,
                                  float right_grip = 0.90f) {
  mecvr::ik::FullBodyInput input;
  input.head.valid = true;
  input.head.position = {0.0, 1.70, 0.0};
  input.left_hand.pose.valid = true;
  input.left_hand.pose.position = {-0.38, 1.25, -0.30};
  input.left_hand.trigger = left_trigger;
  input.left_hand.grip = left_grip;
  input.right_hand.pose.valid = true;
  input.right_hand.pose.position = {0.38, 1.25, -0.30};
  input.right_hand.trigger = right_trigger;
  input.right_hand.grip = right_grip;
  if (rotated) {
    input.right_hand.pose.orientation = {0.0, 0.70710678, 0.0, 0.70710678};
  }
  input.floor_origin = {0.0, 0.0, 0.0};
  input.delta_seconds = 1.0 / 90.0;
  mecvr::ik::FullBodyAnimator animator;
  return animator.update(input);
}

}  // namespace

int main() {
  bool ok = true;
  auto pose = Pose(0.0f, 0.0f, false, 0.0f, 0.0f);
  mecvr::render::BodyOverlayVertex vertices[512]{};
  mecvr::render::BodyOverlayVertex open_vertices[512]{};
  const std::size_t open_count = mecvr::render::BuildBodyOverlayGeometry(
      pose, open_vertices, 512);
  ok &= Check(pose.valid, "source pose valid");
  ok &= Check(open_count > 0 && open_count % 3 == 0,
              "triangle-list geometry generated");
  // The tracked hands in Pose() are below the head. In view-space NDC they
  // must therefore land below the optical center, not at the top of the
  // screen (the regression observed in live Catalyst testing).
  ok &= Check(open_vertices[96].y < 0.0f,
              "hands below head project below view center");
  for (std::size_t i = 0; i < open_count; ++i) {
    ok &= Check(std::isfinite(open_vertices[i].x) &&
                    std::isfinite(open_vertices[i].y) &&
                    std::isfinite(open_vertices[i].depth),
                "finite vertex position");
    ok &= Check(open_vertices[i].x >= -1.0f && open_vertices[i].x <= 1.0f &&
                    open_vertices[i].y >= -1.0f && open_vertices[i].y <= 1.0f,
                "vertex remains inside clip space");
    ok &= Check(open_vertices[i].depth >= 0.0f &&
                    open_vertices[i].depth <= 1.0f,
                "vertex depth cue remains normalized");
  }
  const auto weapon_pose = Pose(1.0f, 1.0f);
  const std::size_t weapon_count =
      mecvr::render::BuildBodyOverlayGeometry(weapon_pose, vertices, 512);
  ok &= Check(weapon_count > open_count, "trigger adds weapon geometry");
  mecvr::render::BodyOverlayVertex closed_vertices[512]{};
  const std::size_t closed_count =
      mecvr::render::BuildBodyOverlayGeometry(
          Pose(0.0f, 0.0f, false, 1.0f, 1.0f), closed_vertices, 512);
  bool grip_changed = false;
  for (std::size_t i = 0; i < open_count && i < closed_count; ++i) {
    if (std::fabs(closed_vertices[i].x - open_vertices[i].x) > 1e-4f ||
        std::fabs(closed_vertices[i].y - open_vertices[i].y) > 1e-4f) {
      grip_changed = true;
      break;
    }
  }
  ok &= Check(grip_changed, "finger geometry follows grip pressure");
  mecvr::render::BodyOverlayVertex rotated_vertices[512]{};
  const std::size_t rotated_count =
      mecvr::render::BuildBodyOverlayGeometry(Pose(1.0f, 1.0f, true),
                                              rotated_vertices, 512);
  bool orientation_changed = false;
  for (std::size_t i = 0; i < weapon_count && i < rotated_count; ++i) {
    if (std::fabs(rotated_vertices[i].x - vertices[i].x) > 1e-4f ||
        std::fabs(rotated_vertices[i].y - vertices[i].y) > 1e-4f) {
      orientation_changed = true;
      break;
    }
  }
  ok &= Check(orientation_changed, "weapon follows hand orientation");

  auto head_turned_pose = weapon_pose;
  head_turned_pose.joints[static_cast<std::size_t>(
      mecvr::ik::BodyJoint::kHead)]
      .orientation = {0.0, 0.70710678118, 0.0, 0.70710678118};
  mecvr::render::BodyOverlayVertex head_turned_vertices[512]{};
  const std::size_t head_turned_count =
      mecvr::render::BuildBodyOverlayGeometry(head_turned_pose,
                                              head_turned_vertices, 512);
  bool head_view_changed = false;
  for (std::size_t i = 0; i < weapon_count && i < head_turned_count; ++i) {
    if (std::fabs(head_turned_vertices[i].x - vertices[i].x) > 1e-4f ||
        std::fabs(head_turned_vertices[i].y - vertices[i].y) > 1e-4f) {
      head_view_changed = true;
      break;
    }
  }
  ok &= Check(head_view_changed,
              "head rotation affects view-local overlay projection");
  mecvr::render::BodyOverlayVertex left_eye_vertices[512]{};
  mecvr::render::BodyOverlayVertex right_eye_vertices[512]{};
  mecvr::render::BodyOverlayView left_view;
  left_view.valid = true;
  left_view.position = {-0.032, 1.70, 0.0};
  left_view.frustum = {-1.1, 0.9, 1.0, -1.0};
  mecvr::render::BodyOverlayView right_view;
  right_view.valid = true;
  right_view.position = {0.032, 1.70, 0.0};
  right_view.frustum = {-0.9, 1.1, 1.0, -1.0};
  const std::size_t left_eye_count = mecvr::render::BuildBodyOverlayGeometry(
      weapon_pose, left_eye_vertices, 512, &left_view);
  const std::size_t right_eye_count = mecvr::render::BuildBodyOverlayGeometry(
      weapon_pose, right_eye_vertices, 512, &right_view);
  double left_eye_mean = 0.0;
  double right_eye_mean = 0.0;
  const std::size_t eye_count =
      std::min(left_eye_count, right_eye_count);
  for (std::size_t i = 0; i < eye_count; ++i) {
    left_eye_mean += left_eye_vertices[i].x;
    right_eye_mean += right_eye_vertices[i].x;
  }
  if (eye_count != 0) {
    left_eye_mean /= static_cast<double>(eye_count);
    right_eye_mean /= static_cast<double>(eye_count);
  }
  ok &= Check(left_eye_count == right_eye_count && eye_count > 0 &&
                  std::fabs(left_eye_mean - right_eye_mean) > 0.01,
              "temporal stereo eyes receive separated overlay projection");

  auto raised_pose = weapon_pose;
  for (auto joint : {mecvr::ik::BodyJoint::kLeftElbow,
                     mecvr::ik::BodyJoint::kLeftWrist,
                     mecvr::ik::BodyJoint::kLeftHand}) {
    raised_pose.joints[static_cast<std::size_t>(joint)].position.y += 0.18;
  }
  mecvr::render::BodyOverlayVertex raised_vertices[512]{};
  const std::size_t raised_count = mecvr::render::BuildBodyOverlayGeometry(
      raised_pose, raised_vertices, 512, &left_view);
  double baseline_arm_y = 0.0;
  double raised_arm_y = 0.0;
  const std::size_t compare_count = std::min(left_eye_count, raised_count);
  for (std::size_t i = 0; i < compare_count; ++i) {
    baseline_arm_y += left_eye_vertices[i].y;
    raised_arm_y += raised_vertices[i].y;
  }
  ok &= Check(compare_count > 0 && raised_arm_y > baseline_arm_y + 0.1,
              "tracked vertical arm movement changes projected geometry");
  const std::size_t invalid_count =
      mecvr::render::BuildBodyOverlayGeometry({}, vertices, 512);
  ok &= Check(invalid_count == 0, "invalid pose produces no geometry");
  std::cout << (ok ? "BODY_OVERLAY_GEOMETRY_TEST: PASS\n"
                   : "BODY_OVERLAY_GEOMETRY_TEST: FAIL\n");
  return ok ? 0 : 1;
}
