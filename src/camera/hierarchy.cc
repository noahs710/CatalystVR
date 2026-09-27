// P2.0 transform-hierarchy implementation. See hierarchy.h.

#include "camera/hierarchy.h"

#include "camera/seam_convert.h"

namespace mecvr::camera {

bool ComposeHierarchy(const HierarchyInput& in,
                      const XRFramePoseSnapshot& snap,
                      std::int64_t now_ns,
                      std::int64_t staleness_budget_ns,
                      HierarchyResult* out) {
  if (out == nullptr) return false;
  if (!SnapshotUsable(snap, now_ns, staleness_budget_ns)) return false;
  out->player_root.orientation = in.player_root_orientation;
  out->player_root.position = in.player_root_position;
  VrCameraPose vr;
  if (!EvaluateVrCamera(in.body_anchor, FromSeam(snap.head.orientation),
                        FromSeam(snap.head.position), in.live_game_orientation,
                        in.live_game_position, snap.space_generation,
                        in.world, &vr)) {
    return false;
  }
  // Comfort Filtering stage: explicit pass-through until M11.
  out->vr_camera.orientation = vr.orientation;
  out->vr_camera.position = vr.position;
  out->comfort_passthrough = true;
  return true;
}

bool EyeViewMatrix(const BodyAnchor& anchor, Quat live_game_orientation,
                   Vec3 live_game_position,
                   const XRFramePoseSnapshot& snap, int eye,
                   const WorldConvention& world, std::int64_t now_ns,
                   std::int64_t staleness_budget_ns, Mat4* view_out) {
  if (view_out == nullptr || (eye != 0 && eye != 1)) return false;
  if (!SnapshotUsable(snap, now_ns, staleness_budget_ns)) return false;
  // Same anchor evaluation as the center head, fed the eye's pose.
  VrCameraPose eye_world;
  if (!EvaluateVrCamera(anchor,
                        FromSeam(snap.views[eye].pose.orientation),
                        FromSeam(snap.views[eye].pose.position),
                        live_game_orientation, live_game_position,
                        snap.space_generation, world, &eye_world)) {
    return false;
  }
  *view_out = MatViewFromPose(eye_world.position, eye_world.orientation);
  return true;
}

}  // namespace mecvr::camera
