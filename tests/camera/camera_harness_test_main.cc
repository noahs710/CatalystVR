// P2.0 camera harness tests: hierarchy composition, eye views,
// trajectories, and graceful-fail paths. Assert-style exe main.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "camera/body_origin.h"
#include "camera/conventions.h"
#include "camera/harness.h"
#include "camera/hierarchy.h"
#include "camera/math.h"
#include "camera/pose_mailbox.h"
#include "camera/seam_convert.h"
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

using mecvr::camera::BodyAnchor;
using mecvr::camera::BuildProjection;
using mecvr::camera::CaptureAnchor;
using mecvr::camera::ComposeHierarchy;
using mecvr::camera::EyeViewMatrix;
using mecvr::camera::FrustumFromSeamFov;
using mecvr::camera::FromSeam;
using mecvr::camera::HarnessEyePair;
using mecvr::camera::HarnessKeyframe;
using mecvr::camera::HierarchyInput;
using mecvr::camera::HierarchyResult;
using mecvr::camera::MakeHarnessSnapshot;
using mecvr::camera::Mat4;
using mecvr::camera::MatInvertRigid;
using mecvr::camera::MatToQuat;
using mecvr::camera::ProjectionConvention;
using mecvr::camera::PoseMailbox;
using mecvr::camera::Quat;
using mecvr::camera::QuatNormalize;
using mecvr::camera::QuatYaw;
using mecvr::camera::RunHarnessFrame;
using mecvr::camera::RunHarnessTrajectory;
using mecvr::camera::ToSeamQuat;
using mecvr::camera::ToSeamVec;
using mecvr::camera::Vec3;
using mecvr::camera::VecSub;
using mecvr::camera::VecLength;
using mecvr::camera::WorldConvention;
using mecvr::camera::XRFramePoseSnapshot;
using mecvr::openxr::XrFovf;

bool Near(double a, double b, double tol = 1e-9) {
  return std::fabs(a - b) <= tol;
}

bool QuatNear(Quat a, Quat b, double tol = 1e-6) {
  const Quat na = QuatNormalize(a);
  const Quat nb = QuatNormalize(b);
  const double d1 = std::fabs(na.x - nb.x) + std::fabs(na.y - nb.y) +
                    std::fabs(na.z - nb.z) + std::fabs(na.w - nb.w);
  const double d2 = std::fabs(na.x + nb.x) + std::fabs(na.y + nb.y) +
                    std::fabs(na.z + nb.z) + std::fabs(na.w + nb.w);
  return (d1 <= tol) || (d2 <= tol);
}

// Eye world position from a view matrix (rigid inverse, translation).
Vec3 EyeWorldPos(const Mat4& view) {
  const Mat4 w = MatInvertRigid(view);
  return Vec3{w.m[3], w.m[7], w.m[11]};
}

double ProjectNdcX(const Mat4& view, const Mat4& projection, Vec3 world) {
  // The Catalyst seam uses row-major storage with column-vector
  // multiplication. Keep this helper deliberately independent from the
  // production projection code so the test catches a shared-sign mistake.
  const double vx = view.m[0] * world.x + view.m[1] * world.y +
                    view.m[2] * world.z + view.m[3];
  const double vy = view.m[4] * world.x + view.m[5] * world.y +
                    view.m[6] * world.z + view.m[7];
  const double vz = view.m[8] * world.x + view.m[9] * world.y +
                    view.m[10] * world.z + view.m[11];
  const double vw = view.m[12] * world.x + view.m[13] * world.y +
                    view.m[14] * world.z + view.m[15];
  const double clip_x = projection.m[0] * vx + projection.m[1] * vy +
                        projection.m[2] * vz + projection.m[3] * vw;
  const double clip_w = projection.m[12] * vx + projection.m[13] * vy +
                        projection.m[14] * vz + projection.m[15] * vw;
  return clip_x / clip_w;
}

XrFovf Sym90() {
  XrFovf f;
  f.angle_left = -0.7853981633974483f;  // -45 deg; tan = -1.
  f.angle_right = 0.7853981633974483f;
  f.angle_up = 0.7853981633974483f;
  f.angle_down = -0.7853981633974483f;
  return f;
}

void TestSnapshotBuilder() {
  HarnessKeyframe key;
  key.time_ns = 1000000;
  key.game_orientation = Quat{0, 0, 0, 1};
  key.game_position = Vec3{0, 0, 0};
  key.hmd_head.orientation = ToSeamQuat(Quat{0, 0, 0, 1});
  key.hmd_head.position = ToSeamVec(Vec3{0.0, 1.6, 0.0});
  const XRFramePoseSnapshot s =
      MakeHarnessSnapshot(key, 3u, 0.032, Sym90(), 1u);
  Check(s.sequence == 3u, "snapshot carries sequence");
  Check(s.position_valid && s.orientation_valid, "snapshot tracked");
  // 1e-6: snapshot poses cross the float seam (float epsilon at these
  // magnitudes is ~2-4e-9; double-side math is covered at 1e-9 in the
  // math suite).
  Check(Near(s.views[0].pose.position.x, -0.032, 1e-6) &&
            Near(s.views[1].pose.position.x, 0.032, 1e-6),
        "eyes offset +/-IPD/2 on head X");
  Check(Near(s.views[0].pose.position.y, 1.6, 1e-6), "eye height = head");
  // Determinism: same keyframe -> identical snapshot.
  const XRFramePoseSnapshot s2 =
      MakeHarnessSnapshot(key, 3u, 0.032, Sym90(), 1u);
  Check(s2.views[0].pose.position.x == s.views[0].pose.position.x &&
            s2.predicted_display_time_ns == s.predicted_display_time_ns,
        "snapshot builder deterministic");
}

void TestStaticEyes() {
  // Identity game + identity HMD, anchored at the same pose: eye
  // views are pure +/-X translations; separation == IPD exactly.
  HarnessKeyframe key;
  key.time_ns = 1000000;
  key.hmd_head.orientation = ToSeamQuat(Quat{0, 0, 0, 1});
  key.hmd_head.position = ToSeamVec(Vec3{0.0, 1.6, 0.0});
  const BodyAnchor anchor =
      CaptureAnchor(Quat{0, 0, 0, 1}, Vec3{0.0, 1.6, 0.0},
                    Quat{0, 0, 0, 1}, 1u);
  WorldConvention world;
  ProjectionConvention proj;
  const XRFramePoseSnapshot snap =
      MakeHarnessSnapshot(key, 1u, 0.032, Sym90(), 1u);
  const HarnessEyePair pair = RunHarnessFrame(
      key, anchor, world, proj, 0.1, 100.0, snap, key.time_ns, 100000000);
  Check(pair.eyes[0].snapshot_used && pair.eyes[1].snapshot_used,
        "static frame uses snapshot for both eyes");
  const Vec3 pl = EyeWorldPos(pair.eyes[0].view);
  const Vec3 pr = EyeWorldPos(pair.eyes[1].view);
  Check(Near(pl.x, -0.032, 1e-6) && Near(pr.x, 0.032, 1e-6),
        "static eyes at +/-IPD/2 in world");
  Check(Near(VecLength(VecSub(pr, pl)), 0.064, 1e-6),
        "static eye separation == IPD");
  // Projection path unity: harness projection == direct builder.
  const Mat4 direct =
      BuildProjection(FrustumFromSeamFov(snap.views[0].fov), 0.1, 100.0, proj);
  bool same = true;
  for (int i = 0; i < 16; ++i) {
    if (!Near(pair.eyes[0].projection.m[i], direct.m[i], 1e-12)) same = false;
  }
  Check(same, "harness projection == direct builder");
}

void TestStereoConvergence() {
  // A world point straight ahead must land at the same normalized horizontal
  // coordinate in both eyes. This exercises the actual eye view plus each
  // eye's asymmetric frustum; checking only IPD or only symmetric FOVs would
  // allow parallel, non-converging stereo to pass.
  HarnessKeyframe key;
  key.time_ns = 1000000;
  key.hmd_head.orientation = ToSeamQuat(Quat{0, 0, 0, 1});
  key.hmd_head.position = ToSeamVec(Vec3{0.0, 1.6, 0.0});
  const BodyAnchor anchor =
      CaptureAnchor(Quat{0, 0, 0, 1}, Vec3{0.0, 1.6, 0.0},
                    Quat{0, 0, 0, 1}, 1u);
  WorldConvention world;
  ProjectionConvention proj;
  XRFramePoseSnapshot snap =
      MakeHarnessSnapshot(key, 1u, 0.032, Sym90(), 1u);
  // At a 3 m target, the left eye needs a slight inward optical-axis shift
  // and the right eye the mirrored shift. These are representative of real
  // HMD per-eye FOVs and make the convergence assertion geometric.
  snap.views[0].fov.angle_left = static_cast<float>(std::atan(-1.0));
  snap.views[0].fov.angle_right = static_cast<float>(std::atan(1.021333333));
  snap.views[1].fov.angle_left = static_cast<float>(std::atan(-1.021333333));
  snap.views[1].fov.angle_right = static_cast<float>(std::atan(1.0));
  const HarnessEyePair pair = RunHarnessFrame(
      key, anchor, world, proj, 0.1, 100.0, snap, key.time_ns, 100000000);
  const Vec3 target{0.0, 1.6, -3.0};
  const double left_x =
      ProjectNdcX(pair.eyes[0].view, pair.eyes[0].projection, target);
  const double right_x =
      ProjectNdcX(pair.eyes[1].view, pair.eyes[1].projection, target);
  Check(Near(left_x, right_x, 1e-5),
        "stereo target converges to one horizontal coordinate");
  Check(Near(left_x, 0.0, 1e-5) && Near(right_x, 0.0, 1e-5),
        "stereo target is centered in both eyes");
}

void TestStereoPosePinning() {
  PoseMailbox mailbox;
  HarnessKeyframe first;
  first.time_ns = 1000000;
  first.hmd_head.orientation = ToSeamQuat(Quat{0, 0, 0, 1});
  first.hmd_head.position = ToSeamVec(Vec3{0.0, 1.6, 0.0});
  const HarnessKeyframe second = first;
  const XRFramePoseSnapshot first_snapshot =
      MakeHarnessSnapshot(first, 41u, 0.032, Sym90(), 1u);
  mailbox.publish(first_snapshot);
  mailbox.publish(MakeHarnessSnapshot(second, 42u, 0.032, Sym90(), 1u));
  XRFramePoseSnapshot pinned;
  Check(mailbox.find(41u, &pinned) && pinned.sequence == 41u,
        "stereo pair resolves its stamped pose");
  Check(mailbox.latest(&pinned) && pinned.sequence == 42u,
        "pose mailbox latest remains current");
  Check(!mailbox.find(7u, &pinned), "unknown stereo pose is rejected");
}

void TestYawTrajectory() {
  // Game fixed at identity; HMD yaws 0 -> 90 in three steps. Eye
  // orientations must follow; separation must stay exactly IPD.
  WorldConvention world;
  ProjectionConvention proj;
  const BodyAnchor anchor =
      CaptureAnchor(Quat{0, 0, 0, 1}, Vec3{0.0, 1.6, 0.0},
                    Quat{0, 0, 0, 1}, 1u);
  const double yaws[] = {0.0, 0.7853981633974483, 1.5707963267948966};
  for (int i = 0; i < 3; ++i) {
    HarnessKeyframe key;
    key.time_ns = 1000000 + i * 13888888;
    key.hmd_head.orientation = ToSeamQuat(QuatYaw(yaws[i]));
    key.hmd_head.position = ToSeamVec(Vec3{0.0, 1.6, 0.0});
    const XRFramePoseSnapshot snap =
        MakeHarnessSnapshot(key, static_cast<std::uint64_t>(i + 1), 0.032,
                            Sym90(), 1u);
    const HarnessEyePair pair = RunHarnessFrame(
        key, anchor, world, proj, 0.1, 100.0, snap, key.time_ns, 100000000);
    char name[80];
    std::snprintf(name, sizeof(name), "yaw step %d both eyes used", i);
    Check(pair.eyes[0].snapshot_used && pair.eyes[1].snapshot_used, name);
    const Vec3 pl = EyeWorldPos(pair.eyes[0].view);
    const Vec3 pr = EyeWorldPos(pair.eyes[1].view);
    std::snprintf(name, sizeof(name), "yaw step %d separation == IPD", i);
    Check(Near(VecLength(VecSub(pr, pl)), 0.064, 1e-6), name);
    // Eye orientation == head yaw (game base identity).
    const Quat el = MatToQuat(MatInvertRigid(pair.eyes[0].view));
    std::snprintf(name, sizeof(name), "yaw step %d eye follows head", i);
    Check(QuatNear(el, QuatYaw(yaws[i]), 1e-6), name);
  }
}

void TestFaithTurnsAndLean() {
  WorldConvention world;
  world.units_per_meter = 2.0;  // 1 m of head = 2 game units.
  ProjectionConvention proj;
  const BodyAnchor anchor =
      CaptureAnchor(Quat{0, 0, 0, 1}, Vec3{0.0, 1.6, 0.0},
                    Quat{0, 0, 0, 1}, 1u);
  // Faith yaws 90 (live game), head fixed: eyes follow the game.
  HarnessKeyframe key;
  key.time_ns = 2000000;
  key.game_orientation = QuatYaw(1.5707963267948966);
  key.game_position = Vec3{5.0, 0.0, 5.0};
  key.hmd_head.orientation = ToSeamQuat(Quat{0, 0, 0, 1});
  key.hmd_head.position = ToSeamVec(Vec3{0.0, 1.6, 0.0});
  XRFramePoseSnapshot snap =
      MakeHarnessSnapshot(key, 1u, 0.032, Sym90(), 1u);
  HarnessEyePair pair = RunHarnessFrame(key, anchor, world, proj, 0.1, 100.0,
                                        snap, key.time_ns, 100000000);
  const Quat el = MatToQuat(MatInvertRigid(pair.eyes[0].view));
  Check(QuatNear(el, QuatYaw(1.5707963267948966), 1e-6),
        "fixed head follows live game yaw");
  // Eye offsets scale with units_per_meter: 0.064 m -> 0.128 units.
  const Vec3 pl = EyeWorldPos(pair.eyes[0].view);
  const Vec3 pr = EyeWorldPos(pair.eyes[1].view);
  Check(Near(VecLength(VecSub(pr, pl)), 0.128, 1e-6),
        "eye separation scales with units-per-meter");
  // Lean: HMD +X 0.1 m with identity game -> +0.2 game units on X.
  HarnessKeyframe lean = key;
  lean.game_orientation = Quat{0, 0, 0, 1};
  lean.game_position = Vec3{0, 0, 0};
  lean.hmd_head.position = ToSeamVec(Vec3{0.1, 1.6, 0.0});
  snap = MakeHarnessSnapshot(lean, 2u, 0.032, Sym90(), 1u);
  pair = RunHarnessFrame(lean, anchor, world, proj, 0.1, 100.0, snap,
                         lean.time_ns, 100000000);
  const Vec3 cl = EyeWorldPos(pair.eyes[0].view);
  Check(Near(cl.x, -0.064 + 0.2, 1e-6),
        "lean shifts eye viewpoint by scaled delta");
}

void TestGracefulFail() {
  WorldConvention world;
  ProjectionConvention proj;
  const BodyAnchor anchor =
      CaptureAnchor(Quat{0, 0, 0, 1}, Vec3{0.0, 1.6, 0.0},
                    Quat{0, 0, 0, 1}, 1u);
  HarnessKeyframe key;
  key.time_ns = 1000000;
  key.hmd_head.orientation = ToSeamQuat(Quat{0, 0, 0, 1});
  key.hmd_head.position = ToSeamVec(Vec3{0.0, 1.6, 0.0});
  XRFramePoseSnapshot snap =
      MakeHarnessSnapshot(key, 1u, 0.032, Sym90(), 1u);
  // Stale: now far past publish + budget.
  HarnessEyePair pair = RunHarnessFrame(key, anchor, world, proj, 0.1, 100.0,
                                        snap, key.time_ns + 500000000, 100);
  Check(!pair.eyes[0].snapshot_used && !pair.eyes[1].snapshot_used,
        "stale snapshot fails gracefully");
  // Untracked.
  snap.position_valid = false;
  pair = RunHarnessFrame(key, anchor, world, proj, 0.1, 100.0, snap,
                         key.time_ns, 100000000);
  Check(!pair.eyes[0].snapshot_used, "untracked snapshot rejected");
  // Generation moved under the anchor.
  snap.position_valid = true;
  snap.space_generation = 2u;
  pair = RunHarnessFrame(key, anchor, world, proj, 0.1, 100.0, snap,
                         key.time_ns, 100000000);
  Check(!pair.eyes[0].snapshot_used, "generation move rejected");
}

void TestTrajectoryAndHierarchy() {
  WorldConvention world;
  ProjectionConvention proj;
  const BodyAnchor anchor =
      CaptureAnchor(Quat{0, 0, 0, 1}, Vec3{0.0, 1.6, 0.0},
                    Quat{0, 0, 0, 1}, 1u);
  std::vector<HarnessKeyframe> keys;
  for (int i = 0; i < 5; ++i) {
    HarnessKeyframe key;
    key.time_ns = 1000000 + i * 13888888;
    key.hmd_head.orientation =
        ToSeamQuat(QuatYaw(0.1 * static_cast<double>(i)));
    key.hmd_head.position =
        ToSeamVec(Vec3{0.02 * static_cast<double>(i), 1.6, 0.0});
    keys.push_back(key);
  }
  const std::vector<HarnessEyePair> run = RunHarnessTrajectory(
      keys, anchor, world, proj, 0.1, 100.0, 0.032, Sym90(), 1u, 100000000);
  Check(run.size() == 5u, "trajectory returns one pair per keyframe");
  bool all_used = true;
  for (const HarnessEyePair& p : run) {
    if (!p.eyes[0].snapshot_used || !p.eyes[1].snapshot_used) all_used = false;
  }
  Check(all_used, "trajectory uses snapshots throughout");
  // Hierarchy echo: player root passes through unmodified, comfort is
  // pass-through, VR camera matches the eye-center expectation.
  HierarchyInput in;
  in.player_root_orientation = QuatYaw(0.3);
  in.player_root_position = Vec3{1.0, 2.0, 3.0};
  in.body_anchor = anchor;
  in.live_game_orientation = Quat{0, 0, 0, 1};
  in.live_game_position = Vec3{0, 0, 0};
  in.world = world;
  const XRFramePoseSnapshot snap =
      MakeHarnessSnapshot(keys[0], 9u, 0.032, Sym90(), 1u);
  HierarchyResult result;
  Check(ComposeHierarchy(in, snap, keys[0].time_ns, 100000000, &result),
        "hierarchy composes");
  Check(QuatNear(result.player_root.orientation, QuatYaw(0.3), 1e-9) &&
            Near(result.player_root.position.x, 1.0),
        "player root echoed unmodified");
  Check(result.comfort_passthrough, "comfort stage is pass-through");
}

}  // namespace

int main() {
  TestSnapshotBuilder();
  TestStaticEyes();
  TestStereoConvergence();
  TestStereoPosePinning();
  TestYawTrajectory();
  TestFaithTurnsAndLean();
  TestGracefulFail();
  TestTrajectoryAndHierarchy();
  if (failures == 0) {
    std::printf("CAMERA_HARNESS_ALL_PASS\n");
    return 0;
  }
  std::printf("CAMERA_HARNESS_FAILURES=%d\n", failures);
  return 1;
}
