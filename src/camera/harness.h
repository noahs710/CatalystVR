#pragma once

// P2.0 fake game-camera harness (spec §23). Scriptable synthetic game
// camera + synthetic HMD trajectories feeding the REAL P2.0 math
// (anchor, hierarchy, projection) to produce expected eye matrices.
// Used by camera_harness_test headless AND later by live M5 validation
// (same functions, live inputs) so the harness prediction is one code
// path, not two.

#include <cstdint>
#include <vector>

#include "camera/conventions.h"
#include "camera/hierarchy.h"
#include "camera/math.h"
#include "camera/snapshot.h"

namespace mecvr::camera {

// One scripted keyframe: game base pose + HMD head pose at a time.
struct HarnessKeyframe {
  std::int64_t time_ns = 0;
  Quat game_orientation;
  Vec3 game_position;
  openxr::XrPosef hmd_head;  // Seam-typed: feeds snapshot construction.
};

// Expected per-eye result for one keyframe.
struct HarnessEyeExpectation {
  Mat4 view;        // World -> eye (row-major storage).
  Mat4 projection;  // Built from the snapshot eye FOV + convention.
  bool snapshot_used = false;  // False = graceful-fail path taken.
};

struct HarnessEyePair {
  HarnessEyeExpectation eyes[2];
};

// Builds a synthetic snapshot for a keyframe: fixed IPD-style eye
// offsets (+/-half_ipd_m on head-local X) and a fixed symmetric FOV.
// Deterministic: same keyframe -> same snapshot, always.
XRFramePoseSnapshot MakeHarnessSnapshot(const HarnessKeyframe& key,
                                        std::uint64_t sequence,
                                        double half_ipd_m,
                                        const openxr::XrFovf& fov,
                                        std::uint64_t space_generation);

// Runs the full chain for one keyframe: snapshot -> hierarchy ->
// per-eye view + projection. anchor/snapshot/now drive the real
// fail-gracefully paths (stale/invalid/generation-moved).
HarnessEyePair RunHarnessFrame(const HarnessKeyframe& key,
                               const BodyAnchor& anchor,
                               const WorldConvention& world,
                               const ProjectionConvention& proj_conv,
                               double near_dist, double far_dist,
                               const XRFramePoseSnapshot& snap,
                               std::int64_t now_ns,
                               std::int64_t staleness_budget_ns);

// Scripted trajectory runner: builds snapshots at keyframe times and
// runs every frame. Returns one eye-pair per keyframe.
std::vector<HarnessEyePair> RunHarnessTrajectory(
    const std::vector<HarnessKeyframe>& keys, const BodyAnchor& anchor,
    const WorldConvention& world, const ProjectionConvention& proj_conv,
    double near_dist, double far_dist, double half_ipd_m,
    const openxr::XrFovf& fov, std::uint64_t space_generation,
    std::int64_t staleness_budget_ns);

}  // namespace mecvr::camera
