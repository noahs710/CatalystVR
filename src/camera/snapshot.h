#pragma once

// P2.0 immutable XR pose snapshot (plan Decision 3, non-negotiable).
//
// The XR worker is the only thread making OpenXR calls. Per XR frame
// it publishes ONE immutable XRFramePoseSnapshot; the camera/render
// thread consumes snapshots and never calls xrLocateViews itself.
// Left and right eye data for one rendered stereo frame MUST come
// from the same located view set / same predicted display time.
//
// Uses the OpenXR seam types (openxr/xr_types.h) so this header needs
// no runtime linkage and stays headless-testable.

#include <cstdint>

#include "camera/math.h"
#include "openxr/xr_types.h"

namespace mecvr::camera {

// One located view set, published immutably. Plain data: the worker
// fills every field from a single xrLocateViews result + frame timing
// and publishes by value (or shared_ptr<const>); consumers copy.
struct XRFramePoseSnapshot {
  std::uint64_t sequence = 0;  // Monotonic per published snapshot.
  std::int64_t predicted_display_time_ns = 0;  // XrTime of the locate.
  std::int64_t predicted_display_period_ns = 0;
  // View-state validity from xrLocateViews (position/sensor tracked).
  bool position_valid = false;
  bool orientation_valid = false;
  // Bumped on every recenter / reference-space regeneration so the
  // camera layer can detect the anchor frame changed under it.
  std::uint64_t space_generation = 0;
  // Viewer origin/centroid (VIEW-space locate at the same time).
  openxr::XrPosef head;
  // The stereo view set IN XRLOCATEVIEWS ORDER (index 0 = left).
  openxr::XrView views[2];
  std::int64_t publish_time_ns = 0;  // Steady-clock publish instant.
};

// True when the snapshot is safe to drive the camera: tracked,
// well-formed, and fresher than the staleness budget. Otherwise the
// caller must fail gracefully to previous-valid-pose / base-camera
// behavior (never apply garbage transforms).
inline bool SnapshotUsable(const XRFramePoseSnapshot& s,
                           std::int64_t now_ns,
                           std::int64_t staleness_budget_ns) {
  if (!s.position_valid || !s.orientation_valid) return false;
  if (s.predicted_display_time_ns <= 0) return false;
  if (now_ns < s.publish_time_ns) return false;  // Clock went backwards.
  return (now_ns - s.publish_time_ns) <= staleness_budget_ns;
}

// True when `next` is a newer publication than `prev` (sequence and
// display-time both advance; display time alone can repeat across
// identical predictions, sequence alone guards reuse ticks).
inline bool SnapshotIsNewer(const XRFramePoseSnapshot& prev,
                            const XRFramePoseSnapshot& next) {
  if (next.sequence <= prev.sequence) return false;
  return next.predicted_display_time_ns >= prev.predicted_display_time_ns;
}

}  // namespace mecvr::camera
