#pragma once

// P2.0 seam converters: OpenXR seam types (float) <-> camera math
// (double). All conversions are exact value copies; precision widens
// float->double and narrows double->float explicitly.

#include "camera/conventions.h"
#include "camera/math.h"
#include "openxr/xr_types.h"

namespace mecvr::camera {

inline Vec3 FromSeam(const openxr::XrVector3f& v) {
  return Vec3{static_cast<double>(v.x), static_cast<double>(v.y),
              static_cast<double>(v.z)};
}

inline Quat FromSeam(const openxr::XrQuaternionf& q) {
  return QuatNormalize(Quat{static_cast<double>(q.x),
                            static_cast<double>(q.y),
                            static_cast<double>(q.z),
                            static_cast<double>(q.w)});
}

inline openxr::XrVector3f ToSeamVec(Vec3 v) {
  return openxr::XrVector3f{static_cast<float>(v.x),
                            static_cast<float>(v.y),
                            static_cast<float>(v.z)};
}

inline openxr::XrQuaternionf ToSeamQuat(Quat q) {
  const Quat n = QuatNormalize(q);
  return openxr::XrQuaternionf{
      static_cast<float>(n.x), static_cast<float>(n.y),
      static_cast<float>(n.z), static_cast<float>(n.w)};
}

// XrFovf angles (radians, left/down negative) -> frustum tangents.
// Sign convention preserved: left/down tangents stay negative.
inline FrustumTangents FrustumFromSeamFov(const openxr::XrFovf& fov) {
  FrustumTangents t;
  t.left = std::tan(static_cast<double>(fov.angle_left));
  t.right = std::tan(static_cast<double>(fov.angle_right));
  t.up = std::tan(static_cast<double>(fov.angle_up));
  t.down = std::tan(static_cast<double>(fov.angle_down));
  return t;
}

}  // namespace mecvr::camera
