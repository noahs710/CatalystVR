#pragma once

// P2.0 camera math core (Sub-project 2). Double-precision vectors,
// quaternions, and 4x4 matrices with EXPLICIT conventions everywhere:
// nothing here assumes row/column-major, handedness, or clip-space
// implicitly — every function documents its convention and every
// convention variant is covered by camera_math_test.
//
// Internal storage: Mat4 is 16 doubles in ROW-MAJOR order
// (m[row*4+col]). Multiplication order (row-vector v*M vs column-
// vector M*v) is a parameter at use sites, never a global assumption.

#include <cmath>
#include <cstdint>

namespace mecvr::camera {

constexpr double kPi = 3.14159265358979323846;

struct Vec3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

struct Quat {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double w = 1.0;
};

struct Mat4 {
  double m[16] = {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                  0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
};

inline double DegToRad(double degrees) { return degrees * (kPi / 180.0); }

// --- Vec3 ---------------------------------------------------------------
inline Vec3 VecAdd(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 VecSub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 VecScale(Vec3 v, double s) { return {v.x * s, v.y * s, v.z * s}; }
inline double VecDot(Vec3 a, Vec3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline double VecLength(Vec3 v) { return std::sqrt(VecDot(v, v)); }

// --- Quat (Hamilton product; q = [x,y,z,w], w last) ----------------------
inline double QuatLength(Quat q) {
  return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
}
inline Quat QuatNormalize(Quat q) {
  const double len = QuatLength(q);
  if (len == 0.0) return Quat{0.0, 0.0, 0.0, 1.0};
  return {q.x / len, q.y / len, q.z / len, q.w / len};
}
inline Quat QuatConjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }

// Hamilton product a*b (apply b first, then a).
inline Quat QuatMul(const Quat& a, const Quat& b) {
  return {
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  };
}

inline Vec3 QuatRotate(const Quat& q, Vec3 v) {
  // v' = q * [v,0] * conj(q), expanded without temporaries.
  const double uvx = q.y * v.z - q.z * v.y;
  const double uvy = q.z * v.x - q.x * v.z;
  const double uvz = q.x * v.y - q.y * v.x;
  const double w2 = q.w * 2.0;
  return {
      v.x + w2 * uvx + 2.0 * (q.y * uvz - q.z * uvy),
      v.y + w2 * uvy + 2.0 * (q.z * uvx - q.x * uvz),
      v.z + w2 * uvz + 2.0 * (q.x * uvy - q.y * uvx),
  };
}

inline Quat QuatFromAxisAngle(Vec3 axis, double angle_rad) {
  const double len = VecLength(axis);
  if (len == 0.0) return Quat{0.0, 0.0, 0.0, 1.0};
  const double half = angle_rad * 0.5;
  const double s = std::sin(half) / len;
  return {axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
}

// Yaw about +Y in a right-handed Y-up frame (OpenXR VIEW/LOCAL).
inline Quat QuatYaw(double yaw_rad) {
  return QuatFromAxisAngle(Vec3{0.0, 1.0, 0.0}, yaw_rad);
}

// Rotation part of a Mat4 (rows/cols 0-2) -> quaternion (Shepperd).
// Used for basis changes; the matrix must be (near-)orthonormal.
Quat MatToQuat(const Mat4& m);

// Extract yaw/pitch/roll (YXZ order) for TEST ASSERTIONS ONLY —
// production code stays in quaternion space (no Euler singularities).
void QuatToYawPitchRoll(const Quat& q, double* yaw, double* pitch,
                        double* roll);

// Wrap to [-pi, pi]. Used only for readable test comparisons.
inline double WrapPi(double a) {
  while (a > kPi) a -= 2.0 * kPi;
  while (a < -kPi) a += 2.0 * kPi;
  return a;
}

// --- Mat4 (row-major storage) -------------------------------------------
inline Mat4 MatIdentity() { return Mat4{}; }

// Standard matrix product C = A*B in storage order (valid for both
// vector conventions: the convention lives in how vectors multiply).
Mat4 MatMul(const Mat4& a, const Mat4& b);

inline Mat4 MatTranspose(const Mat4& m) {
  Mat4 out;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out.m[r * 4 + c] = m.m[c * 4 + r];
  return out;
}

// Rigid-body inverse (rotation + translation only; no scale/shear).
// Valid for view matrices and pose compositions.
Mat4 MatInvertRigid(const Mat4& m);

// General 4x4 inverse (Gauss-Jordan). Needed for projection/VP
// inverses in the M3a taxonomy (inverse view/projection/VP).
// Returns false when singular (out untouched).
bool MatInvertGeneral(const Mat4& m, Mat4* out);

// Rigid transform from position + orientation: maps LOCAL points to
// the PARENT frame (pose matrix). Row-major storage; use with
// MatTransformPoint with matching row_vector flag.
Mat4 MatFromPose(Vec3 position, const Quat& orientation);

// View matrix (parent/world -> local/camera frame): rigid inverse of
// the camera pose matrix.
inline Mat4 MatViewFromPose(Vec3 position, const Quat& orientation) {
  return MatInvertRigid(MatFromPose(position, orientation));
}

struct Vec4 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double w = 1.0;
};

// Transform a point. row_vector=true:  p' = p * M (D3D-style rows);
// false: p' = M * p (OpenGL-style columns). Storage is row-major
// either way; the flag selects the multiplication, nothing else.
Vec4 MatTransformPoint(const Mat4& m, Vec4 p, bool row_vector);

}  // namespace mecvr::camera
