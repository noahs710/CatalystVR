// P2.0 camera math core implementation. See math.h for conventions.

#include "camera/math.h"

namespace mecvr::camera {

void QuatToYawPitchRoll(const Quat& q, double* yaw, double* pitch,
                        double* roll) {
  // YXZ order, right-handed Y-up. Test assertions only.
  const Quat n = QuatNormalize(q);
  const double sinp = 2.0 * (n.w * n.x - n.y * n.z);
  double p = 0.0;
  if (sinp >= 1.0) {
    p = kPi * 0.5;
  } else if (sinp <= -1.0) {
    p = -kPi * 0.5;
  } else {
    p = std::asin(sinp);
  }
  double y = 0.0;
  double r = 0.0;
  const double cp = std::cos(p);
  if (cp > 1e-9) {
    y = std::atan2(2.0 * (n.w * n.y + n.x * n.z),
                   1.0 - 2.0 * (n.x * n.x + n.y * n.y));
    r = std::atan2(2.0 * (n.w * n.z + n.x * n.y),
                   1.0 - 2.0 * (n.x * n.x + n.z * n.z));
  } else {
    // Gimbal lock: fold roll into yaw.
    y = std::atan2(2.0 * (n.w * n.y - n.x * n.z),
                   1.0 - 2.0 * (n.y * n.y + n.z * n.z));
    r = 0.0;
  }
  if (yaw != nullptr) *yaw = y;
  if (pitch != nullptr) *pitch = p;
  if (roll != nullptr) *roll = r;
}

Quat MatToQuat(const Mat4& m) {
  const double trace =
      m.m[0] + m.m[5] + m.m[10];  // Rotation-part trace.
  Quat q;
  if (trace > 0.0) {
    const double s = std::sqrt(trace + 1.0) * 2.0;
    q.w = 0.25 * s;
    q.x = (m.m[9] - m.m[6]) / s;
    q.y = (m.m[2] - m.m[8]) / s;
    q.z = (m.m[4] - m.m[1]) / s;
  } else if (m.m[0] > m.m[5] && m.m[0] > m.m[10]) {
    const double s = std::sqrt(1.0 + m.m[0] - m.m[5] - m.m[10]) * 2.0;
    q.w = (m.m[9] - m.m[6]) / s;
    q.x = 0.25 * s;
    q.y = (m.m[1] + m.m[4]) / s;
    q.z = (m.m[2] + m.m[8]) / s;
  } else if (m.m[5] > m.m[10]) {
    const double s = std::sqrt(1.0 + m.m[5] - m.m[0] - m.m[10]) * 2.0;
    q.w = (m.m[2] - m.m[8]) / s;
    q.x = (m.m[1] + m.m[4]) / s;
    q.y = 0.25 * s;
    q.z = (m.m[6] + m.m[9]) / s;
  } else {
    const double s = std::sqrt(1.0 + m.m[10] - m.m[0] - m.m[5]) * 2.0;
    q.w = (m.m[4] - m.m[1]) / s;
    q.x = (m.m[2] + m.m[8]) / s;
    q.y = (m.m[6] + m.m[9]) / s;
    q.z = 0.25 * s;
  }
  return QuatNormalize(q);
}

Mat4 MatMul(const Mat4& a, const Mat4& b) {
  Mat4 out;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      out.m[r * 4 + c] = a.m[r * 4 + 0] * b.m[0 * 4 + c] +
                         a.m[r * 4 + 1] * b.m[1 * 4 + c] +
                         a.m[r * 4 + 2] * b.m[2 * 4 + c] +
                         a.m[r * 4 + 3] * b.m[3 * 4 + c];
    }
  }
  return out;
}

Mat4 MatInvertRigid(const Mat4& m) {
  // Rotation rows become columns; translation negated through them.
  // Bottom row must be (0,0,0,1) — rigid transforms only.
  Mat4 out;
  out.m[0] = m.m[0];
  out.m[1] = m.m[4];
  out.m[2] = m.m[8];
  out.m[3] = 0.0;
  out.m[4] = m.m[1];
  out.m[5] = m.m[5];
  out.m[6] = m.m[9];
  out.m[7] = 0.0;
  out.m[8] = m.m[2];
  out.m[9] = m.m[6];
  out.m[10] = m.m[10];
  out.m[11] = 0.0;
  // t' = -R^T * t. Layout matches MatFromPose: translation in the
  // last column (m[3], m[7], m[11]), bottom row (0,0,0,1).
  const double tx = m.m[3];
  const double ty = m.m[7];
  const double tz = m.m[11];
  out.m[3] = -(out.m[0] * tx + out.m[1] * ty + out.m[2] * tz);
  out.m[7] = -(out.m[4] * tx + out.m[5] * ty + out.m[6] * tz);
  out.m[11] = -(out.m[8] * tx + out.m[9] * ty + out.m[10] * tz);
  out.m[12] = 0.0;
  out.m[13] = 0.0;
  out.m[14] = 0.0;
  out.m[15] = 1.0;
  return out;
}

bool MatInvertGeneral(const Mat4& m, Mat4* out) {
  if (out == nullptr) return false;
  // Gauss-Jordan on a [4x8] augmented system, partial pivoting.
  double a[4][8];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) a[r][c] = m.m[r * 4 + c];
    for (int c = 4; c < 8; ++c) a[r][c] = (c - 4 == r) ? 1.0 : 0.0;
  }
  for (int col = 0; col < 4; ++col) {
    int pivot = col;
    double best = std::fabs(a[col][col]);
    for (int r = col + 1; r < 4; ++r) {
      const double v = std::fabs(a[r][col]);
      if (v > best) {
        best = v;
        pivot = r;
      }
    }
    if (best < 1e-12) return false;  // Singular.
    if (pivot != col) {
      for (int c = 0; c < 8; ++c) {
        const double t = a[col][c];
        a[col][c] = a[pivot][c];
        a[pivot][c] = t;
      }
    }
    const double inv = 1.0 / a[col][col];
    for (int c = 0; c < 8; ++c) a[col][c] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == col) continue;
      const double f = a[r][col];
      if (f != 0.0) {
        for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
      }
    }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out->m[r * 4 + c] = a[r][c + 4];
  return true;
}

Mat4 MatFromPose(Vec3 position, const Quat& orientation) {
  // Column-vector layout in row-major storage: basis vectors in
  // columns 0-2, translation in column 3, bottom row (0,0,0,1).
  // Matches MatInvertRigid and MatTransformPoint(row_vector=false).
  const Quat q = QuatNormalize(orientation);
  const double xx = q.x * q.x;
  const double yy = q.y * q.y;
  const double zz = q.z * q.z;
  const double xy = q.x * q.y;
  const double xz = q.x * q.z;
  const double yz = q.y * q.z;
  const double wx = q.w * q.x;
  const double wy = q.w * q.y;
  const double wz = q.w * q.z;
  Mat4 out;
  out.m[0] = 1.0 - 2.0 * (yy + zz);
  out.m[1] = 2.0 * (xy - wz);
  out.m[2] = 2.0 * (xz + wy);
  out.m[3] = position.x;
  out.m[4] = 2.0 * (xy + wz);
  out.m[5] = 1.0 - 2.0 * (xx + zz);
  out.m[6] = 2.0 * (yz - wx);
  out.m[7] = position.y;
  out.m[8] = 2.0 * (xz - wy);
  out.m[9] = 2.0 * (yz + wx);
  out.m[10] = 1.0 - 2.0 * (xx + yy);
  out.m[11] = position.z;
  out.m[12] = 0.0;
  out.m[13] = 0.0;
  out.m[14] = 0.0;
  out.m[15] = 1.0;
  return out;
}

Vec4 MatTransformPoint(const Mat4& m, Vec4 p, bool row_vector) {
  Vec4 out;
  if (row_vector) {
    // p' = p * M.
    out.x = p.x * m.m[0] + p.y * m.m[4] + p.z * m.m[8] + p.w * m.m[12];
    out.y = p.x * m.m[1] + p.y * m.m[5] + p.z * m.m[9] + p.w * m.m[13];
    out.z = p.x * m.m[2] + p.y * m.m[6] + p.z * m.m[10] + p.w * m.m[14];
    out.w = p.x * m.m[3] + p.y * m.m[7] + p.z * m.m[11] + p.w * m.m[15];
  } else {
    // p' = M * p.
    out.x = m.m[0] * p.x + m.m[1] * p.y + m.m[2] * p.z + m.m[3] * p.w;
    out.y = m.m[4] * p.x + m.m[5] * p.y + m.m[6] * p.z + m.m[7] * p.w;
    out.z = m.m[8] * p.x + m.m[9] * p.y + m.m[10] * p.z + m.m[11] * p.w;
    out.w = m.m[12] * p.x + m.m[13] * p.y + m.m[14] * p.z + m.m[15] * p.w;
  }
  return out;
}

}  // namespace mecvr::camera
