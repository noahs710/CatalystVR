// P2.0 camera math tests: core math, projection convention matrix,
// snapshot contract, anchor/recenter math. Assert-style exe main.
#include <cmath>
#include <cstdio>

#include "camera/body_origin.h"
#include "camera/conventions.h"
#include "camera/math.h"
#include "camera/snapshot.h"

namespace {

int failures = 0;
void Check(bool ok, const char* name) {
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", name);
  } else {
    std::printf("ok: %s\n", name);
  }
}

using mecvr::camera::AnchorRotationDelta;
using mecvr::camera::BodyAnchor;
using mecvr::camera::BuildProjection;
using mecvr::camera::CaptureAnchor;
using mecvr::camera::ClipZ;
using mecvr::camera::DegToRad;
using mecvr::camera::EvaluateVrCamera;
using mecvr::camera::FrustumTangents;
using mecvr::camera::Handedness;
using mecvr::camera::Mat4;
using mecvr::camera::MatFromPose;
using mecvr::camera::MatIdentity;
using mecvr::camera::MatInvertGeneral;
using mecvr::camera::MatInvertRigid;
using mecvr::camera::MatMul;
using mecvr::camera::MatToQuat;
using mecvr::camera::MatTransformPoint;
using mecvr::camera::MatTranspose;
using mecvr::camera::MultOrder;
using mecvr::camera::PackMatrix;
using mecvr::camera::ProjectionConvention;
using mecvr::camera::Quat;
using mecvr::camera::QuatBasisChange;
using mecvr::camera::QuatFromAxisAngle;
using mecvr::camera::QuatMul;
using mecvr::camera::QuatNormalize;
using mecvr::camera::QuatRotate;
using mecvr::camera::QuatToYawPitchRoll;
using mecvr::camera::QuatYaw;
using mecvr::camera::SnapshotIsNewer;
using mecvr::camera::SnapshotUsable;
using mecvr::camera::Vec3;
using mecvr::camera::Vec4;
using mecvr::camera::VecAdd;
using mecvr::camera::VecLength;
using mecvr::camera::VecSub;
using mecvr::camera::VrCameraPose;
using mecvr::camera::WorldConvention;
using mecvr::camera::XrOffsetToGame;
using mecvr::camera::XRFramePoseSnapshot;
using mecvr::camera::kPi;

bool Near(double a, double b, double tol = 1e-9) {
  return std::fabs(a - b) <= tol;
}

bool QuatNear(Quat a, Quat b, double tol = 1e-9) {
  // Sign-agnostic: q and -q are the same rotation.
  const Quat na = QuatNormalize(a);
  const Quat nb = QuatNormalize(b);
  const double d1 = std::fabs(na.x - nb.x) + std::fabs(na.y - nb.y) +
                    std::fabs(na.z - nb.z) + std::fabs(na.w - nb.w);
  const double d2 = std::fabs(na.x + nb.x) + std::fabs(na.y + nb.y) +
                    std::fabs(na.z + nb.z) + std::fabs(na.w + nb.w);
  return (d1 <= tol) || (d2 <= tol);
}

bool MatNear(const Mat4& a, const Mat4& b, double tol = 1e-9) {
  for (int i = 0; i < 16; ++i) {
    if (!Near(a.m[i], b.m[i], tol)) return false;
  }
  return true;
}

void TestQuatCore() {
  const Quat yaw90 = QuatYaw(DegToRad(90.0));
  const Vec3 fwd{0.0, 0.0, -1.0};
  const Vec3 turned = QuatRotate(yaw90, fwd);
  // +90 deg about +Y sends -Z to -X (right-handed).
  Check(Near(turned.x, -1.0) && Near(turned.y, 0.0) && Near(turned.z, 0.0),
        "yaw90 rotates -Z to -X");
  const Quat back = QuatMul(QuatYaw(DegToRad(-90.0)), yaw90);
  Check(QuatNear(back, Quat{0.0, 0.0, 0.0, 1.0}), "yaw composes to identity");
  double yaw = 0.0, pitch = 0.0, roll = 0.0;
  QuatToYawPitchRoll(QuatMul(QuatYaw(0.5), QuatFromAxisAngle({1, 0, 0}, 0.25)),
                     &yaw, &pitch, &roll);
  Check(Near(yaw, 0.5, 1e-6) && Near(pitch, 0.25, 1e-6) &&
            Near(roll, 0.0, 1e-6),
        "ypr extraction round-trips YXZ");
  // MatToQuat round-trips.
  const Quat qs[] = {QuatYaw(0.7),
                     QuatFromAxisAngle({0.3, 0.8, 0.1}, 1.1),
                     QuatFromAxisAngle({0, 0, 1}, 2.9)};
  for (int i = 0; i < 3; ++i) {
    char name[64];
    std::snprintf(name, sizeof(name), "mat-quat round-trip %d", i);
    Check(QuatNear(MatToQuat(MatFromPose({0, 0, 0}, qs[i])), qs[i], 1e-9),
          name);
  }
}

void TestMatCore() {
  const Quat q = QuatMul(QuatYaw(0.6), QuatFromAxisAngle({1, 0, 0}, -0.2));
  const Vec3 p{3.0, -1.5, 7.25};
  const Mat4 pose = MatFromPose(p, q);
  const Mat4 view = MatInvertRigid(pose);
  const Vec4 back = MatTransformPoint(view, MatTransformPoint(pose, {1, 2, 3, 1}, false), false);
  Check(Near(back.x, 1.0) && Near(back.y, 2.0) && Near(back.z, 3.0) &&
            Near(back.w, 1.0),
        "pose then view is identity on points");
  Mat4 gen;
  Check(MatInvertGeneral(pose, &gen), "general inverse succeeds (rigid)");
  Check(MatNear(gen, view, 1e-9), "general inverse matches rigid inverse");
  // General inverse on a non-rigid matrix (projection-like).
  ProjectionConvention conv;
  const FrustumTangents f{-1.0, 1.0, 1.0, -1.0};
  const Mat4 proj = BuildProjection(f, 0.1, 100.0, conv);
  Mat4 pinv;
  Check(MatInvertGeneral(proj, &pinv), "general inverse succeeds (proj)");
  Check(MatNear(MatMul(proj, pinv), MatIdentity(), 1e-6),
        "proj * proj^-1 is identity");
  Mat4 singular;
  for (int i = 0; i < 16; ++i) singular.m[i] = 0.0;
  Check(!MatInvertGeneral(singular, &gen), "singular matrix rejected");
  // Row-vector vs column-vector consistency: p*M_row == (M_col*p)
  // when M_row = transpose(M_col).
  const Mat4 pr = MatTranspose(proj);
  const Vec4 v{0.5, -0.25, -3.0, 1.0};
  const Vec4 a = MatTransformPoint(proj, v, false);
  const Vec4 b = MatTransformPoint(pr, v, true);
  Check(Near(a.x, b.x, 1e-9) && Near(a.y, b.y, 1e-9) &&
            Near(a.z, b.z, 1e-9) && Near(a.w, b.w, 1e-9),
        "row-M^T equals col-M on points");
}

void TestProjectionCore() {
  ProjectionConvention conv;  // col-vector, RH, [0,1], std, finite.
  // Symmetric 90-degree frustum, n=0.1, f=100.
  const FrustumTangents sym{-1.0, 1.0, 1.0, -1.0};
  const Mat4 p = BuildProjection(sym, 0.1, 100.0, conv);
  auto ndc = [&](Vec4 v) {
    const Vec4 c = MatTransformPoint(p, v, false);
    return Vec4{c.x / c.w, c.y / c.w, c.z / c.w, 1.0};
  };
  const Vec4 at_near = ndc({0.0, 0.0, -0.1, 1.0});
  Check(Near(at_near.z, 0.0, 1e-9), "sym: near plane maps to NDC 0");
  const Vec4 at_far = ndc({0.0, 0.0, -100.0, 1.0});
  Check(Near(at_far.z, 1.0, 1e-9), "sym: far plane maps to NDC 1");
  // Hand-derived mid value: z=-2 -> (200/99.9 - 10/99.9)/2 = 190/199.8.
  const Vec4 mid = ndc({0.0, 0.0, -2.0, 1.0});
  Check(Near(mid.x, 0.0) && Near(mid.y, 0.0) &&
            Near(mid.z, 190.0 / 199.8, 1e-9),
        "sym: z=-2 maps to hand-derived NDC");
  // Asymmetric frustum: l=-1, r=3 (center ray at x=+1 per unit depth).
  const FrustumTangents asym{-1.0, 3.0, 2.0, -1.0};
  const Mat4 pa = BuildProjection(asym, 0.1, 100.0, conv);
  auto ndca = [&](Vec4 v) {
    const Vec4 c = MatTransformPoint(pa, v, false);
    return Vec4{c.x / c.w, c.y / c.w, c.z / c.w, 1.0};
  };
  // At depth 2: left edge x=-2 -> -1, right edge x=6 -> +1, center x=2->0.
  Check(Near(ndca({-2.0, 0.0, -2.0, 1.0}).x, -1.0, 1e-9),
        "asym: left edge maps to -1");
  Check(Near(ndca({6.0, 0.0, -2.0, 1.0}).x, 1.0, 1e-9),
        "asym: right edge maps to +1");
  Check(Near(ndca({2.0, 0.0, -2.0, 1.0}).x, 0.0, 1e-9),
        "asym: center ray maps to 0");
  // Vertical: u=2, d=-1; at depth 2: top y=4 -> +1, bottom y=-2 -> -1.
  Check(Near(ndca({2.0, 4.0, -2.0, 1.0}).y, 1.0, 1e-9),
        "asym: top edge maps to +1");
  Check(Near(ndca({2.0, -2.0, -2.0, 1.0}).y, -1.0, 1e-9),
        "asym: bottom edge maps to -1");
  // Degenerate inputs fail safe to identity.
  Check(MatNear(BuildProjection(sym, 0.0, 100.0, conv), MatIdentity()),
        "zero near fails safe");
  Check(MatNear(BuildProjection(sym, 10.0, 5.0, conv), MatIdentity()),
        "far<near fails safe");
  const FrustumTangents flat{1.0, 1.0, 1.0, -1.0};
  Check(MatNear(BuildProjection(flat, 0.1, 100.0, conv), MatIdentity()),
        "zero-width frustum fails safe");
}

// Full convention matrix: every combination must map near/far to the
// exact expected NDC depth. Catches sign/transpose/remap errors.
void TestConventionMatrix() {
  const FrustumTangents sym{-1.0, 1.0, 1.0, -1.0};
  int combos = 0;
  for (int mult = 0; mult < 2; ++mult) {
    for (int clip = 0; clip < 2; ++clip) {
      for (int hand = 0; hand < 2; ++hand) {
        for (int rev = 0; rev < 2; ++rev) {
          for (int inf = 0; inf < 2; ++inf) {
            ProjectionConvention conv;
            conv.mult = (mult == 0) ? MultOrder::kColumnVector
                                    : MultOrder::kRowVector;
            conv.clip_z = (clip == 0) ? ClipZ::kZeroToOne
                                      : ClipZ::kNegOneToOne;
            conv.handed =
                (hand == 0) ? Handedness::kRight : Handedness::kLeft;
            conv.reverse_z = (rev == 1);
            conv.infinite_far = (inf == 1);
            const Mat4 m = BuildProjection(sym, 0.1, 100.0, conv);
            const bool row_vec = (mult == 1);
            // View-space probe depths: RH looks along -Z, LH along +Z.
            const double zn = (hand == 0) ? -0.1 : 0.1;
            const double zf = (hand == 0) ? -100.0 : 100.0;
            const double zvery = (hand == 0) ? -1.0e6 : 1.0e6;
            auto depth_of = [&](double z) {
              const Vec4 c =
                  MatTransformPoint(m, {0.0, 0.0, z, 1.0}, row_vec);
              return c.z / c.w;
            };
            // Expected NDC depths per convention.
            double want_near = 0.0, want_far = 1.0;
            if (conv.reverse_z) {
              want_near = 1.0;
              want_far = 0.0;
            }
            if (conv.clip_z == ClipZ::kNegOneToOne) {
              want_near = want_near * 2.0 - 1.0;
              want_far = want_far * 2.0 - 1.0;
            }
            char name[128];
            std::snprintf(name, sizeof(name),
                          "conv mult=%d clip=%d hand=%d rev=%d inf=%d near",
                          mult, clip, hand, rev, inf);
            Check(Near(depth_of(zn), want_near, 1e-9), name);
            std::snprintf(name, sizeof(name),
                          "conv mult=%d clip=%d hand=%d rev=%d inf=%d far",
                          mult, clip, hand, rev, inf);
            if (conv.infinite_far) {
              Check(Near(depth_of(zvery), want_far, 1e-4), name);
            } else {
              Check(Near(depth_of(zf), want_far, 1e-9), name);
            }
            ++combos;
          }
        }
      }
    }
  }
  Check(combos == 32, "all 32 convention combos exercised");
}

void TestPackAndWorld() {
  ProjectionConvention conv;
  const FrustumTangents f{-1.0, 3.0, 2.0, -1.0};
  const Mat4 m = BuildProjection(f, 0.1, 100.0, conv);
  float row[16], col[16];
  PackMatrix(m, true, row);
  PackMatrix(m, false, col);
  bool ok = true;
  for (int i = 0; i < 16; ++i) {
    if (!Near(row[i], m.m[i], 1e-6)) ok = false;
  }
  Check(ok, "row-major pack is identity");
  ok = true;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      if (!Near(col[r * 4 + c], m.m[c * 4 + r], 1e-6)) ok = false;
    }
  Check(ok, "column-major pack is transpose");
  WorldConvention world;
  world.units_per_meter = 2.0;
  const Vec3 g = XrOffsetToGame({0.5, 0.0, 0.0}, world);
  Check(Near(g.x, 1.0) && Near(g.y, 0.0) && Near(g.z, 0.0),
        "units-per-meter scales offsets");
}

void TestSnapshot() {
  XRFramePoseSnapshot s;
  s.sequence = 5;
  s.predicted_display_time_ns = 1000;
  s.predicted_display_period_ns = 13888888;
  s.position_valid = true;
  s.orientation_valid = true;
  s.publish_time_ns = 900;
  Check(SnapshotUsable(s, 950, 100), "fresh tracked snapshot usable");
  Check(!SnapshotUsable(s, 950 + 101, 100), "stale snapshot rejected");
  XRFramePoseSnapshot bad = s;
  bad.position_valid = false;
  Check(!SnapshotUsable(bad, 950, 100), "untracked snapshot rejected");
  Check(!SnapshotUsable(s, 800, 100), "backwards clock rejected");
  XRFramePoseSnapshot next = s;
  next.sequence = 6;
  next.predicted_display_time_ns = 1100;
  Check(SnapshotIsNewer(s, next), "sequence+time advance is newer");
  Check(!SnapshotIsNewer(next, s), "older snapshot not newer");
  Check(!SnapshotIsNewer(s, s), "same snapshot not newer");
}

void TestAnchor() {
  WorldConvention world;  // Identity: 1 unit = 1 m, same axes.
  // Identity recenter: no HMD motion -> VR camera == live game base.
  BodyAnchor a =
      CaptureAnchor(Quat{0, 0, 0, 1}, Vec3{1.0, 1.6, 0.5},
                    QuatYaw(DegToRad(30.0)), 7u);
  VrCameraPose out;
  const Quat live_game = QuatYaw(DegToRad(30.0));
  Check(EvaluateVrCamera(a, Quat{0, 0, 0, 1}, Vec3{1.0, 1.6, 0.5},
                         live_game, Vec3{10, 2, 20}, 7u, world, &out),
        "identity delta evaluates");
  Check(QuatNear(out.orientation, live_game), "identity delta keeps base ori");
  Check(Near(out.position.x, 10.0) && Near(out.position.y, 2.0) &&
            Near(out.position.z, 20.0),
        "identity delta keeps base pos");
  // Arbitrary initial yaw: anchor at 150 deg, HMD turns +30 -> the VR
  // camera yaws exactly +30 from live game base, whatever the anchor.
  BodyAnchor a150 = CaptureAnchor(QuatYaw(DegToRad(150.0)), Vec3{0, 1.6, 0},
                                  Quat{0, 0, 0, 1}, 1u);
  Check(EvaluateVrCamera(a150, QuatYaw(DegToRad(180.0)), Vec3{0, 1.6, 0},
                         Quat{0, 0, 0, 1}, Vec3{0, 0, 0}, 1u, world, &out),
        "arbitrary-yaw delta evaluates");
  Check(QuatNear(out.orientation, QuatYaw(DegToRad(30.0)), 1e-9),
        "arbitrary initial yaw yields exact +30 delta");
  // Yaw wraparound: anchor -179, current +179 -> delta -2 (short way).
  BodyAnchor aw = CaptureAnchor(QuatYaw(DegToRad(-179.0)), Vec3{0, 1.6, 0},
                                Quat{0, 0, 0, 1}, 1u);
  const Quat dw = AnchorRotationDelta(aw, QuatYaw(DegToRad(179.0)));
  double yaw = 0, pitch = 0, roll = 0;
  QuatToYawPitchRoll(dw, &yaw, &pitch, &roll);
  Check(Near(yaw, DegToRad(-2.0), 1e-9), "wraparound takes short way (-2)");
  // Pitch/roll deltas pass through in game-local axes.
  BodyAnchor ap = CaptureAnchor(Quat{0, 0, 0, 1}, Vec3{0, 1.6, 0},
                                Quat{0, 0, 0, 1}, 1u);
  const Quat pr = QuatMul(QuatFromAxisAngle({1, 0, 0}, 0.3),
                          QuatFromAxisAngle({0, 0, 1}, -0.1));
  Check(EvaluateVrCamera(ap, pr, Vec3{0, 1.6, 0}, Quat{0, 0, 0, 1},
                         Vec3{0, 0, 0}, 1u, world, &out),
        "pitch/roll delta evaluates");
  Check(QuatNear(out.orientation, pr, 1e-9), "pitch/roll delta exact");
  // Live game yaw changes: Faith turns 90 with a fixed head -> the VR
  // view follows Faith (delta identity, base moved).
  Check(EvaluateVrCamera(ap, Quat{0, 0, 0, 1}, Vec3{0, 1.6, 0},
                         QuatYaw(DegToRad(90.0)), Vec3{5, 0, 5}, 1u, world,
                         &out),
        "fixed head + live game turn evaluates");
  Check(QuatNear(out.orientation, QuatYaw(DegToRad(90.0)), 1e-9),
        "view follows live game yaw");
  // Translated LOCAL origin: anchor at (10,1.6,10), HMD leans +X 0.2
  // with identity game base -> viewpoint shifts +X 0.2.
  BodyAnchor at = CaptureAnchor(Quat{0, 0, 0, 1}, Vec3{10.0, 1.6, 10.0},
                                Quat{0, 0, 0, 1}, 1u);
  Check(EvaluateVrCamera(at, Quat{0, 0, 0, 1}, Vec3{10.2, 1.6, 10.0},
                         Quat{0, 0, 0, 1}, Vec3{0, 0, 0}, 1u, world, &out),
        "translated-origin lean evaluates");
  Check(Near(out.position.x, 0.2, 1e-9) && Near(out.position.y, 0.0) &&
            Near(out.position.z, 0.0, 1e-9),
        "lean measured from anchor, not origin");
  // Lean rotates with the live game base: game yawed 90, HMD leans
  // anchor-local +X -> game-world -Z... game yaw +90 maps local +X to
  // world: R_y(90) * (1,0,0) = (cos90, 0, -sin90) = (0,0,-1). Yes -Z.
  Check(EvaluateVrCamera(at, Quat{0, 0, 0, 1}, Vec3{10.2, 1.6, 10.0},
                         QuatYaw(DegToRad(90.0)), Vec3{0, 0, 0}, 1u, world,
                         &out),
        "lean under turned game evaluates");
  Check(Near(out.position.x, 0.0, 1e-9) && Near(out.position.y, 0.0) &&
            Near(out.position.z, -0.2, 1e-9),
        "lean resolves through live game yaw");
  // Anchor-localized lean: head yawed 90 at anchor AND now (pure yaw
  // offset, no translation change except anchor-local +X 0.2): the
  // lean must still read as anchor-local +X, not world +X.
  BodyAnchor ay = CaptureAnchor(QuatYaw(DegToRad(90.0)), Vec3{0, 1.6, 0},
                                Quat{0, 0, 0, 1}, 1u);
  // World-frame +X 0.2 under a yaw-90 anchor = anchor-local -Z... the
  // anchor-localized delta of world (0.2,0,0) by yaw(-90):
  // R_y(-90)*(0.2,0,0) = (cos(-90)*0.2, 0, -sin(-90)*0.2) = (0,0,0.2).
  Check(EvaluateVrCamera(ay, QuatYaw(DegToRad(90.0)), Vec3{0.2, 1.6, 0},
                         Quat{0, 0, 0, 1}, Vec3{0, 0, 0}, 1u, world, &out),
        "yawed-anchor lean evaluates");
  Check(Near(out.position.x, 0.0, 1e-9) && Near(out.position.z, 0.2, 1e-9),
        "lean localized to anchor frame");
  // Generation moved -> fail gracefully (false, out untouched).
  VrCameraPose sentinel;
  sentinel.position = Vec3{99, 99, 99};
  sentinel.orientation = Quat{0, 0, 0, 1};
  out = sentinel;
  Check(!EvaluateVrCamera(a, Quat{0, 0, 0, 1}, Vec3{1.0, 1.6, 0.5},
                          live_game, Vec3{10, 2, 20}, 8u, world, &out),
        "generation move fails gracefully");
  Check(Near(out.position.x, 99.0), "failed eval leaves output untouched");
  BodyAnchor invalid;
  Check(!EvaluateVrCamera(invalid, Quat{0, 0, 0, 1}, Vec3{0, 0, 0},
                          live_game, Vec3{0, 0, 0}, 1u, world, &out),
        "invalid anchor fails gracefully");
  // Axis-map basis change: L = yaw(+90). An HMD yaw delta (+30 about
  // head-local Y) conjugated through L stays +30 about Y (yaw axis
  // preserved), while a pitch delta (+20 about X) becomes +20 about
  // the mapped axis: L*Rx*L^-1 = rotation about L(X) = -Z... R_y(90)
  // maps +X to (0,0,-1): pitch becomes roll. Verify exactly.
  BodyAnchor ab = CaptureAnchor(Quat{0, 0, 0, 1}, Vec3{0, 1.6, 0},
                                Quat{0, 0, 0, 1}, 1u);
  ab.local_axis_map = MatFromPose({0, 0, 0}, QuatYaw(DegToRad(90.0)));
  ab.has_axis_map = true;
  const Quat pitch20 = QuatFromAxisAngle({1, 0, 0}, DegToRad(20.0));
  const Quat mapped = QuatBasisChange(pitch20, ab.local_axis_map);
  const Quat expect_roll =
      QuatFromAxisAngle({0, 0, -1}, DegToRad(20.0));
  Check(QuatNear(mapped, expect_roll, 1e-9),
        "basis change maps pitch axis through L");
}

}  // namespace

int main() {
  TestQuatCore();
  TestMatCore();
  TestProjectionCore();
  TestConventionMatrix();
  TestPackAndWorld();
  TestSnapshot();
  TestAnchor();
  if (failures == 0) {
    std::printf("CAMERA_MATH_ALL_PASS\n");
    return 0;
  }
  std::printf("CAMERA_MATH_FAILURES=%d\n", failures);
  return 1;
}
