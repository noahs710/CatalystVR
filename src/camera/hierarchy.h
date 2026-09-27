#pragma once

// P2.0 transform-hierarchy composer (spec §5 chain):
//   World -> Player Root -> VR Body Origin -> Game Camera Base
//     -> Comfort Filtering -> HMD Local Pose -> Eye Pose
//
// Comfort Filtering is an explicit PASS-THROUGH stage: sub-project 4
// (M11) fills it; M3–M6 prove the chain with comfort neutral. This
// module composes rigid poses only; projection is built separately
// (conventions.h) from the same-snapshot XrView FOV.

#include "camera/body_origin.h"
#include "camera/conventions.h"
#include "camera/math.h"
#include "camera/snapshot.h"

namespace mecvr::camera {

// One evaluated stage (game-world frame), for tests and diagnostics.
struct HierarchyPose {
  Quat orientation;
  Vec3 position;
};

// Full chain inputs. Player-root and game-base are LIVE game values;
// the snapshot is the immutable XR frame (Decision 3).
struct HierarchyInput {
  Quat player_root_orientation;
  Vec3 player_root_position;
  BodyAnchor body_anchor;  // VR Body Origin stage.
  Quat live_game_orientation;
  Vec3 live_game_position;
  WorldConvention world;
};

// Composes the chain up to the VR camera (pre-eye) for one eye:
// player root is informational (validates Faith's frame, never
// modified); body anchor supplies the HMD delta; comfort passes
// through. Returns false when the anchor/snapshot is unusable
// (caller fails gracefully to base-camera behavior).
struct HierarchyResult {
  HierarchyPose player_root;  // Echoed (never modified by this plan).
  HierarchyPose vr_camera;    // Game base x HMD delta (render only).
  bool comfort_passthrough = true;  // Always true until M11.
};

bool ComposeHierarchy(const HierarchyInput& in,
                      const XRFramePoseSnapshot& snap,
                      std::int64_t now_ns,
                      std::int64_t staleness_budget_ns,
                      HierarchyResult* out);

// Per-eye view matrix (world -> eye frame) for one snapshot eye.
// The eye pose runs through the SAME anchor evaluation as the center
// head (EvaluateVrCamera with the eye's snapshot pose as input), so
// center and eyes share one code path, one snapshot, one units
// convention — there is no separate mixed-frame eye composition to
// get wrong. (Decision 3: same snapshot, both eyes, always.)
// Returns false when the snapshot is unusable.
bool EyeViewMatrix(const BodyAnchor& anchor, Quat live_game_orientation,
                   Vec3 live_game_position,
                   const XRFramePoseSnapshot& snap, int eye /*0,1*/,
                   const WorldConvention& world, std::int64_t now_ns,
                   std::int64_t staleness_budget_ns, Mat4* view_out);

}  // namespace mecvr::camera
