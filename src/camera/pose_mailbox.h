#pragma once

#include <mutex>

#include "camera/snapshot.h"

namespace mecvr::camera {

// Cross-thread handoff from the XR worker to the render hook. The render
// thread copies one immutable snapshot; it never calls OpenXR.
class PoseMailbox {
 public:
  void publish(const XRFramePoseSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = snapshot;
    has_snapshot_ = true;
  }

  bool latest(XRFramePoseSnapshot* out) const {
    if (out == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_snapshot_) return false;
    *out = snapshot_;
    return true;
  }

 private:
  mutable std::mutex mutex_;
  XRFramePoseSnapshot snapshot_;
  bool has_snapshot_ = false;
};

}  // namespace mecvr::camera
