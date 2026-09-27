// P2.0 fake game-camera harness implementation. See harness.h.

#include "camera/harness.h"

#include "camera/seam_convert.h"

namespace mecvr::camera {

XRFramePoseSnapshot MakeHarnessSnapshot(const HarnessKeyframe& key,
                                        std::uint64_t sequence,
                                        double half_ipd_m,
                                        const openxr::XrFovf& fov,
                                        std::uint64_t space_generation) {
  XRFramePoseSnapshot snap;
  snap.sequence = sequence;
  snap.predicted_display_time_ns = key.time_ns + 1000000;  // +1 ms lookahead.
  snap.predicted_display_period_ns = 13888888;  // 72 Hz nominal.
  snap.position_valid = true;
  snap.orientation_valid = true;
  snap.space_generation = space_generation;
  snap.head = key.hmd_head;
  // Eyes: head pose translated +/-half_ipd on head-local X.
  const Quat hq = FromSeam(key.hmd_head.orientation);
  const Vec3 hp = FromSeam(key.hmd_head.position);
  for (int eye = 0; eye < 2; ++eye) {
    const double s = (eye == 0) ? -half_ipd_m : half_ipd_m;
    const Vec3 off = QuatRotate(hq, Vec3{s, 0.0, 0.0});
    snap.views[eye].pose.orientation = key.hmd_head.orientation;
    snap.views[eye].pose.position = ToSeamVec(VecAdd(hp, off));
    snap.views[eye].fov = fov;
  }
  snap.publish_time_ns = key.time_ns;
  return snap;
}

HarnessEyePair RunHarnessFrame(const HarnessKeyframe& key,
                               const BodyAnchor& anchor,
                               const WorldConvention& world,
                               const ProjectionConvention& proj_conv,
                               double near_dist, double far_dist,
                               const XRFramePoseSnapshot& snap,
                               std::int64_t now_ns,
                               std::int64_t staleness_budget_ns) {
  HarnessEyePair pair;
  HierarchyInput in;
  in.player_root_orientation = key.game_orientation;
  in.player_root_position = key.game_position;
  in.body_anchor = anchor;
  in.live_game_orientation = key.game_orientation;
  in.live_game_position = key.game_position;
  in.world = world;
  HierarchyResult result;
  if (!ComposeHierarchy(in, snap, now_ns, staleness_budget_ns, &result)) {
    return pair;  // snapshot_used=false: graceful-fail path. Eyes zero.
  }
  // View matrices are rigid (convention-neutral); the projection
  // carries the caller's mult convention (built per eye below).
  for (int eye = 0; eye < 2; ++eye) {
    Mat4 view;
    if (!EyeViewMatrix(anchor, key.game_orientation, key.game_position,
                       snap, eye, world, now_ns, staleness_budget_ns,
                       &view)) {
      continue;
    }
    pair.eyes[eye].view = view;
    pair.eyes[eye].projection =
        BuildProjection(FrustumFromSeamFov(snap.views[eye].fov), near_dist,
                        far_dist, proj_conv);
    pair.eyes[eye].snapshot_used = true;
  }
  return pair;
}

std::vector<HarnessEyePair> RunHarnessTrajectory(
    const std::vector<HarnessKeyframe>& keys, const BodyAnchor& anchor,
    const WorldConvention& world, const ProjectionConvention& proj_conv,
    double near_dist, double far_dist, double half_ipd_m,
    const openxr::XrFovf& fov, std::uint64_t space_generation,
    std::int64_t staleness_budget_ns) {
  std::vector<HarnessEyePair> out;
  out.reserve(keys.size());
  std::uint64_t seq = 1;
  for (const HarnessKeyframe& key : keys) {
    const XRFramePoseSnapshot snap = MakeHarnessSnapshot(
        key, seq++, half_ipd_m, fov, space_generation);
    out.push_back(RunHarnessFrame(key, anchor, world, proj_conv, near_dist,
                                  far_dist, snap, key.time_ns,
                                  staleness_budget_ns));
  }
  return out;
}

}  // namespace mecvr::camera
