#include "camera/view_override.h"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::printf("FAIL: %s\n", message);
    ++failures;
  }
}

bool Near(double a, double b) { return std::fabs(a - b) < 1e-5; }

}  // namespace

int main() {
  using namespace mecvr::camera;
  const Mat4 base = MatFromPose(Vec3{4.0, 2.0, -7.0}, Quat{});
  const Quat yaw = QuatYaw(DegToRad(30.0));
  const Mat4 rotated = ApplyViewRotationDelta(
      MatViewFromPose(Vec3{4.0, 2.0, -7.0}, Quat{}), yaw,
      MultOrder::kColumnVector);
  const Mat4 rotated_pose = MatInvertRigid(rotated);
  Check(Near(rotated_pose.m[3], 4.0) && Near(rotated_pose.m[7], 2.0) &&
            Near(rotated_pose.m[11], -7.0),
        "rotation preserves camera position");

  float cb[48] = {};
  for (int i = 0; i < 16; ++i) cb[8 + i] = static_cast<float>(
      MatViewFromPose(Vec3{4.0, 2.0, -7.0}, Quat{}).m[i]);
  Check(RewriteViewMatrix(cb, 48, 8, yaw, MultOrder::kColumnVector),
        "valid view buffer rewrites");
  Check(Near(cb[8 + 15], 1.0), "rewritten view remains affine");

  const Mat4 translated = ApplyViewPoseDelta(
      MatViewFromPose(Vec3{4.0, 2.0, -7.0}, Quat{}), Quat{},
      Vec3{0.1, 0.0, 0.0}, 10.0, MultOrder::kColumnVector);
  const Mat4 translated_pose = MatInvertRigid(translated);
  Check(Near(translated_pose.m[3], 5.0) && Near(translated_pose.m[7], 2.0) &&
            Near(translated_pose.m[11], -7.0),
        "translation uses calibrated game units");

  float bad[24] = {};
  Check(!RewriteViewMatrix(bad, 24, 8, yaw, MultOrder::kColumnVector),
        "non-view buffer fails closed");
  Check(Near(base.m[3], 4.0), "base matrix remains immutable");

  ProjectionConvention projection_convention;
  projection_convention.infinite_far = true;
  float projection_cb[32] = {};
  const Mat4 desktop_projection = BuildProjection(
      FrustumTangents{-1.6, 1.6, 0.9, -0.9}, 0.06, 1.0,
      projection_convention);
  for (int i = 0; i < 16; ++i)
    projection_cb[8 + i] = static_cast<float>(desktop_projection.m[i]);
  const FrustumTangents left_eye{-1.1, 0.9, 1.0, -1.0};
  Check(RewriteProjectionMatrix(projection_cb, 32, 8, left_eye),
        "valid Catalyst projection rewrites");
  const Mat4 expected =
      BuildProjection(left_eye, 0.06, 1.0, projection_convention);
  bool projection_matches = true;
  for (int i = 0; i < 16; ++i)
    projection_matches &= Near(projection_cb[8 + i], expected.m[i]);
  Check(projection_matches, "runtime asymmetric frustum is written exactly");
  Check(Near(projection_cb[8 + 11], -0.06),
        "projection rewrite preserves game near plane");
  float invalid_projection[24] = {};
  invalid_projection[8] = 1.0f;
  invalid_projection[8 + 5] = 1.0f;
  invalid_projection[8 + 15] = 1.0f;
  Check(!RewriteProjectionMatrix(invalid_projection, 24, 8, left_eye),
        "non-projection buffer fails closed");

  // The runtime camera path must keep applying pose motion even when a
  // renderer variant rejects the projection signature. This mirrors the
  // production split between view-pose and projection compatibility guards.
  float pose_with_unknown_projection[32] = {};
  const Mat4 valid_view =
      MatViewFromPose(Vec3{0.0, 1.6, -2.0}, Quat{});
  for (int i = 0; i < 16; ++i)
    pose_with_unknown_projection[i] = static_cast<float>(valid_view.m[i]);
  pose_with_unknown_projection[16 + 0] = 0.0f;
  pose_with_unknown_projection[16 + 5] = 0.0f;
  pose_with_unknown_projection[16 + 15] = 0.0f;
  Check(RewriteViewPoseMatrix(
            pose_with_unknown_projection, 32, 0, yaw, Vec3{0.0, 0.1, 0.0},
            100.0, MultOrder::kColumnVector),
        "view pose survives projection compatibility miss");

  if (failures == 0) std::printf("view_override: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
