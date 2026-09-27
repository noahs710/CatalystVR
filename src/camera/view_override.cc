#include "camera/view_override.h"

#include <cmath>

namespace mecvr::camera {
namespace {

bool Near(double a, double b, double eps) {
  return std::fabs(a - b) <= eps;
}

bool LooksLikeView(const Mat4& m) {
  const double n0 = m.m[0] * m.m[0] + m.m[1] * m.m[1] + m.m[2] * m.m[2];
  const double n1 = m.m[4] * m.m[4] + m.m[5] * m.m[5] + m.m[6] * m.m[6];
  const double n2 = m.m[8] * m.m[8] + m.m[9] * m.m[9] + m.m[10] * m.m[10];
  const double d01 = m.m[0] * m.m[4] + m.m[1] * m.m[5] + m.m[2] * m.m[6];
  const double d02 = m.m[0] * m.m[8] + m.m[1] * m.m[9] + m.m[2] * m.m[10];
  const double d12 = m.m[4] * m.m[8] + m.m[5] * m.m[9] + m.m[6] * m.m[10];
  return Near(n0, 1.0, 0.03) && Near(n1, 1.0, 0.03) &&
         Near(n2, 1.0, 0.03) && Near(d01, 0.0, 0.06) &&
         Near(d02, 0.0, 0.06) && Near(d12, 0.0, 0.06) &&
         Near(m.m[15], 1.0, 1e-3) && Near(m.m[12], 0.0, 1e-3) &&
         Near(m.m[13], 0.0, 1e-3) && Near(m.m[14], 0.0, 1e-3);
}

bool LooksLikeCatalystProjection(const Mat4& m) {
  return std::isfinite(m.m[0]) && std::isfinite(m.m[5]) &&
         m.m[0] > 0.05 && m.m[5] > 0.05 &&
         Near(m.m[1], 0.0, 1e-4) && Near(m.m[3], 0.0, 1e-4) &&
         Near(m.m[4], 0.0, 1e-4) && Near(m.m[7], 0.0, 1e-4) &&
         Near(m.m[8], 0.0, 1e-4) && Near(m.m[9], 0.0, 1e-4) &&
         Near(m.m[10], -1.0, 0.02) && m.m[11] < -0.001 &&
         Near(m.m[12], 0.0, 1e-4) && Near(m.m[13], 0.0, 1e-4) &&
         Near(m.m[14], -1.0, 1e-3) && Near(m.m[15], 0.0, 1e-4);
}

}  // namespace

Mat4 ApplyViewRotationDelta(const Mat4& base_view, const Quat& delta,
                            MultOrder order) {
  const Mat4 inverse_delta = MatFromPose(Vec3{}, QuatConjugate(delta));
  if (order == MultOrder::kColumnVector) {
    return MatMul(inverse_delta, base_view);
  }
  return MatMul(base_view, inverse_delta);
}

Mat4 ApplyViewPoseDelta(const Mat4& base_view, const Quat& delta,
                        Vec3 local_translation_m, double units_per_meter,
                        MultOrder order) {
  if (order == MultOrder::kRowVector) {
    // The retail bridge is column-vector, but keep the pure seam explicit:
    // transpose into the canonical column-vector representation, compose,
    // then transpose back.
    const Mat4 canonical = MatTranspose(base_view);
    const Mat4 result = ApplyViewPoseDelta(
        canonical, delta, local_translation_m, units_per_meter,
        MultOrder::kColumnVector);
    return MatTranspose(result);
  }
  const Mat4 base_pose = MatInvertRigid(base_view);
  const Quat base_orientation = MatToQuat(base_pose);
  const Vec3 world_offset = QuatRotate(
      base_orientation, VecScale(local_translation_m, units_per_meter));
  const Vec3 position = VecAdd(
      Vec3{base_pose.m[3], base_pose.m[7], base_pose.m[11]}, world_offset);
  const Quat orientation =
      QuatNormalize(QuatMul(base_orientation, QuatNormalize(delta)));
  return MatViewFromPose(position, orientation);
}

bool RewriteViewMatrix(float* floats, std::size_t float_count,
                       std::size_t view_offset_floats, const Quat& delta,
                       MultOrder order) {
  return RewriteViewPoseMatrix(floats, float_count, view_offset_floats, delta,
                               Vec3{}, 0.0, order);
}

bool RewriteViewPoseMatrix(float* floats, std::size_t float_count,
                           std::size_t view_offset_floats, const Quat& delta,
                           Vec3 local_translation_m, double units_per_meter,
                           MultOrder order) {
  if (floats == nullptr || view_offset_floats > float_count ||
      float_count - view_offset_floats < 16) {
    return false;
  }
  Mat4 base;
  for (int i = 0; i < 16; ++i) {
    base.m[i] = static_cast<double>(floats[view_offset_floats + i]);
  }
  if (!LooksLikeView(base)) return false;
  const Mat4 result = ApplyViewPoseDelta(base, delta, local_translation_m,
                                         units_per_meter, order);
  for (int i = 0; i < 16; ++i) {
    floats[view_offset_floats + i] = static_cast<float>(result.m[i]);
  }
  return true;
}

bool RewriteProjectionMatrix(float* floats, std::size_t float_count,
                             std::size_t projection_offset_floats,
                             const FrustumTangents& frustum) {
  if (floats == nullptr || projection_offset_floats > float_count ||
      float_count - projection_offset_floats < 16) {
    return false;
  }
  Mat4 base;
  for (int i = 0; i < 16; ++i)
    base.m[i] = floats[projection_offset_floats + i];
  if (!LooksLikeCatalystProjection(base)) return false;
  ProjectionConvention convention;
  convention.mult = MultOrder::kColumnVector;
  convention.clip_z = ClipZ::kZeroToOne;
  convention.handed = Handedness::kRight;
  convention.infinite_far = true;
  const Mat4 result = BuildProjection(frustum, -base.m[11], 1.0, convention);
  if (Near(result.m[15], 1.0, 1e-6)) return false;
  for (int i = 0; i < 16; ++i)
    floats[projection_offset_floats + i] = static_cast<float>(result.m[i]);
  return true;
}

}  // namespace mecvr::camera
