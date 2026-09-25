#pragma once

// Minimal OpenXR-mapped types for the M1A MockXR backend (plan T5).
//
// No OpenXR SDK headers are used here: the structs below mirror the subset
// of <openxr/openxr.h> the backend needs, so MockXRBackend compiles and runs
// with zero linkage to a real runtime. Each type notes its real-API
// counterpart for the M1B RealOpenXRBackend implementation.

#include <cmath>
#include <cstdint>

namespace mecvr::openxr {

// Nanosecond timestamp. Maps to XrTime.
using XrTime = std::int64_t;
inline constexpr XrTime kNanosecondsPerSecond = 1000000000LL;

// Maps to XrVector3f.
struct XrVector3f {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

// Maps to XrQuaternionf (x, y, z, w order).
struct XrQuaternionf {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
};

// Maps to XrPosef.
struct XrPosef {
  XrQuaternionf orientation;
  XrVector3f position;
};

// View frustum angles in radians. Maps to XrFovf. Left/down are negative.
struct XrFovf {
  float angle_left = 0.0f;
  float angle_right = 0.0f;
  float angle_up = 0.0f;
  float angle_down = 0.0f;
};

// Per-eye render pose. Subset of XrView (pose + fov; drops type/next).
struct XrView {
  XrPosef pose;
  XrFovf fov;
};

inline bool operator==(const XrVector3f& a, const XrVector3f& b) {
  return a.x == b.x && a.y == b.y && a.z == b.z;
}

inline bool operator==(const XrQuaternionf& a, const XrQuaternionf& b) {
  return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

inline bool operator==(const XrPosef& a, const XrPosef& b) {
  return a.orientation == b.orientation && a.position == b.position;
}

inline XrPosef IdentityPose() {
  return XrPosef{XrQuaternionf{0.0f, 0.0f, 0.0f, 1.0f},
                 XrVector3f{0.0f, 0.0f, 0.0f}};
}

inline float DegreesToRadians(float degrees) {
  return degrees * (3.14159265358979323846f / 180.0f);
}

inline XrQuaternionf YawQuaternion(float yaw_radians) {
  const float half = yaw_radians * 0.5f;
  return XrQuaternionf{0.0f, std::sin(half), 0.0f, std::cos(half)};
}

inline XrQuaternionf Conjugate(const XrQuaternionf& q) {
  return XrQuaternionf{-q.x, -q.y, -q.z, q.w};
}

inline XrQuaternionf Multiply(const XrQuaternionf& a,
                              const XrQuaternionf& b) {
  return XrQuaternionf{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                       a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                       a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                       a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
  const XrQuaternionf vq{v.x, v.y, v.z, 0.0f};
  const XrQuaternionf turned = Multiply(Multiply(q, vq), Conjugate(q));
  return XrVector3f{turned.x, turned.y, turned.z};
}

// Pose of b expressed relative to a: identity when a == b.
inline XrPosef RelativePose(const XrPosef& a, const XrPosef& b) {
  const XrQuaternionf a_inv = Conjugate(a.orientation);
  XrPosef out;
  out.orientation = Multiply(a_inv, b.orientation);
  const XrVector3f delta{b.position.x - a.position.x,
                         b.position.y - a.position.y,
                         b.position.z - a.position.z};
  out.position = Rotate(a_inv, delta);
  return out;
}

// Composition: origin applied to local. Maps chained-space evaluation.
inline XrPosef ComposePose(const XrPosef& origin, const XrPosef& local) {
  XrPosef out;
  out.orientation = Multiply(origin.orientation, local.orientation);
  const XrVector3f turned = Rotate(origin.orientation, local.position);
  out.position = XrVector3f{origin.position.x + turned.x,
                            origin.position.y + turned.y,
                            origin.position.z + turned.z};
  return out;
}

}  // namespace mecvr::openxr
