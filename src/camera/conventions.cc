// P2.0 projection/world conventions implementation.

#include "camera/conventions.h"

namespace mecvr::camera {

Mat4 BuildProjection(const FrustumTangents& frustum, double near_dist,
                     double far_dist, const ProjectionConvention& conv) {
  // Degenerate input guards: identity out (caller validates).
  if (!(near_dist > 0.0)) return MatIdentity();
  if (!conv.infinite_far && !(far_dist > near_dist)) return MatIdentity();
  const double width = frustum.right - frustum.left;
  const double height = frustum.up - frustum.down;
  if (width == 0.0 || height == 0.0) return MatIdentity();

  // Canonical build is column-vector (M*p), right-handed, D3D [0,1]
  // depth; every other convention is derived from it by exact
  // transforms below, so all variants share one tested core.
  //
  // P = [ 2/w, 0,   -(r+l)/w, 0
  //       0,   2/h, -(u+d)/h, 0
  //       0,   0,   A,        B
  //       0,   0,   -1,       0 ]
  // View space: camera looks along -Z (right-handed). A point at
  // z=-n must map to NDC z=0 (std) or 1 (reverse); z=-f to 1 or 0.
  // Solved: std A=-f/(f-n), B=-f*n/(f-n); reverse A=n/(f-n),
  // B=f*n/(f-n). Infinite far: std A=-1, B=-n; reverse A=0, B=n.
  const double sx = 2.0 / width;
  const double sy = 2.0 / height;
  // Positive: NDC=-1 at the left edge requires sx*l - ox = -1, i.e.
  // ox = (r+l)/(r-l). (Verified against D3DXMatrixPerspectiveOffCenterRH
  // and asymmetric hand-derived cases in camera_math_test.)
  const double ox = (frustum.right + frustum.left) / width;
  const double oy = (frustum.up + frustum.down) / height;
  double a = 0.0;
  double b = 0.0;
  if (conv.infinite_far) {
    if (conv.reverse_z) {
      a = 0.0;
      b = near_dist;
    } else {
      a = -1.0;
      b = -near_dist;
    }
  } else if (conv.reverse_z) {
    a = near_dist / (far_dist - near_dist);
    b = far_dist * near_dist / (far_dist - near_dist);
  } else {
    a = -far_dist / (far_dist - near_dist);
    b = -far_dist * near_dist / (far_dist - near_dist);
  }
  Mat4 m;
  m.m[0] = sx;
  m.m[1] = 0.0;
  m.m[2] = ox;
  m.m[3] = 0.0;
  m.m[4] = 0.0;
  m.m[5] = sy;
  m.m[6] = oy;
  m.m[7] = 0.0;
  m.m[8] = 0.0;
  m.m[9] = 0.0;
  m.m[10] = a;
  m.m[11] = b;
  m.m[12] = 0.0;
  m.m[13] = 0.0;
  m.m[14] = -1.0;
  m.m[15] = 0.0;

  if (conv.clip_z == ClipZ::kNegOneToOne) {
    // Remap depth row: NDC' = 2*NDC - 1. Row 2 (z) becomes
    // 2*(a*z + b) - w, i.e. A' = 2a+1... derived: z'_clip =
    // 2*(a*z_eye + b*w) - w_eye where w_eye = -z_eye... careful:
    // NDC z = (a*z + b*w)/(-z) with w=1. NDC' = 2*NDC-1 =
    // (2a*z + 2b + z)/(-z) = ((2a+1)*z + 2b)/(-z).
    // So A' = 2a+1, B' = 2b, w-row unchanged.
    m.m[10] = 2.0 * a + 1.0;
    m.m[11] = 2.0 * b;
  }
  if (conv.handed == Handedness::kLeft) {
    // Left-handed view space looks along +Z: negate the z input
    // column (col 2 of rows 0-2) and the w-row z element... exact
    // transform: P_lh = P_rh * S where S = diag(1,1,-1,1).
    // Column 2 of P gets negated.
    m.m[2] = -m.m[2];
    m.m[6] = -m.m[6];
    m.m[10] = -m.m[10];
    m.m[14] = -m.m[14];
  }
  if (conv.mult == MultOrder::kRowVector) {
    // Row-vector form is the transpose: p*M_row == (M_col*p)^T.
    m = MatTranspose(m);
  }
  return m;
}

void PackMatrix(const Mat4& m, bool storage_row_major, float* out16) {
  if (out16 == nullptr) return;
  if (storage_row_major) {
    for (int i = 0; i < 16; ++i)
      out16[i] = static_cast<float>(m.m[i]);
  } else {
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        out16[r * 4 + c] = static_cast<float>(m.m[c * 4 + r]);
  }
}

Vec3 XrOffsetToGame(Vec3 xr_meters, const WorldConvention& conv) {
  const double* k = conv.axis_map;
  Vec3 game;
  game.x = k[0] * xr_meters.x + k[1] * xr_meters.y + k[2] * xr_meters.z;
  game.y = k[3] * xr_meters.x + k[4] * xr_meters.y + k[5] * xr_meters.z;
  game.z = k[6] * xr_meters.x + k[7] * xr_meters.y + k[8] * xr_meters.z;
  return VecScale(game, conv.units_per_meter);
}

}  // namespace mecvr::camera
