#pragma once

#include <array>
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
    history_[history_cursor_] = snapshot;
    history_cursor_ = (history_cursor_ + 1) % history_.size();
    has_snapshot_ = true;
  }

  bool latest(XRFramePoseSnapshot* out) const {
    if (out == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_snapshot_) return false;
    *out = snapshot_;
    return true;
  }

  // Temporal stereo captures two consecutive game presents, so both eyes
  // must resolve the exact XR sample that opened the pair rather than racing
  // against a newer worker sample. The bounded history keeps this lookup
  // allocation-free and covers normal present-to-present latency.
  bool find(std::uint64_t sequence, XRFramePoseSnapshot* out) const {
    if (out == nullptr || sequence == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const XRFramePoseSnapshot& candidate : history_) {
      if (candidate.sequence == sequence) {
        *out = candidate;
        return true;
      }
    }
    return false;
  }

 private:
  static constexpr std::size_t kHistorySize = 8;
  mutable std::mutex mutex_;
  XRFramePoseSnapshot snapshot_;
  std::array<XRFramePoseSnapshot, kHistorySize> history_{};
  std::size_t history_cursor_ = 0;
  bool has_snapshot_ = false;
};

}  // namespace mecvr::camera
