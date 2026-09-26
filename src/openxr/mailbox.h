#pragma once

// T11 bounded frame mailbox / copy-request handoff (plan Key Decision 3,
// render stream). The ONLY channel between the game Present thread and
// the XR frame worker.
//
// Contract:
//  - Game thread calls tryPublish() at the proven M2A capture point.
//    tryPublish takes one short mutex (no condition wait, no XR call, no
//    unbounded wait) so the Present thread NEVER blocks on xrWaitFrame
//    or any XR timing. Live path: the game thread has already finished
//    its D3D11 CopyResource on the game immediate context before
//    publishing; the published MonoFrame is immutable (shared read-only
//    with the XR worker — the CPU model of fence-shared staging).
//  - XR worker calls consumeNewest() once per XR tick. Newest safe
//    completed frame wins: older unpublished frames are superseded
//    (counted), never queued for late display.
//  - Bounded: capacity fixed at construction (default 2); a full mailbox
//    drops the oldest, never grows, never allocates on the XR tick
//    beyond the frame storage itself.
//  - Shutdown: drain() empties the mailbox with a bounded lock; the
//    worker drains, then the observer unhooks (Key Decision 3 ordering).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "render/m2b_mono.h"

namespace mecvr::openxr {

// Copy-request view of a mailbox slot: in the live D3D path the game
// thread posts (sequence, capture_time) once its staging copy is
// complete, and the XR worker uploads from the fence-shared texture.
// Here the request travels WITH its immutable pixels.
struct CopyRequest {
  std::uint64_t sequence = 0;
  std::int64_t capture_time_ns = 0;
};

inline CopyRequest RequestFor(const render::MonoFrame& frame) {
  CopyRequest request;
  request.sequence = frame.sequence;
  request.capture_time_ns = frame.capture_time_ns;
  return request;
}

class FrameMailbox {
 public:
  explicit FrameMailbox(std::size_t capacity = 2) : capacity_(capacity < 1 ? 1 : capacity) {}

  FrameMailbox(const FrameMailbox&) = delete;
  FrameMailbox& operator=(const FrameMailbox&) = delete;

  // Game thread. Returns false only for a null frame or after shutdown;
  // never waits on XR state.
  bool tryPublish(render::MonoFramePtr frame) {
    if (!frame) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return false;
    if (queue_.size() >= capacity_) {
      queue_.pop_front();
      ++superseded_;
    }
    queue_.push_back(frame);
    ++published_;
    if (queue_.size() > high_water_) high_water_ = queue_.size();
    return true;
  }

  // XR worker. Returns the newest published frame, or null when empty.
  // Skipped older frames count as superseded.
  render::MonoFramePtr consumeNewest() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return nullptr;
    render::MonoFramePtr newest = queue_.back();
    superseded_ += queue_.size() - 1;
    consumed_new_ += 1;
    queue_.clear();
    return newest;
  }

  // Bounded shutdown drain. Returns frames discarded.
  std::uint64_t drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = true;
    const std::uint64_t dropped = queue_.size();
    drained_ += dropped;
    queue_.clear();
    return dropped;
  }

  std::size_t depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }
  std::size_t highWater() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return high_water_;
  }
  std::uint64_t published() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return published_;
  }
  std::uint64_t consumedNew() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return consumed_new_;
  }
  std::uint64_t superseded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return superseded_;
  }
  std::uint64_t drained() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return drained_;
  }

 private:
  mutable std::mutex mutex_;
  std::deque<render::MonoFramePtr> queue_;
  std::size_t capacity_;
  std::size_t high_water_ = 0;
  std::uint64_t published_ = 0;
  std::uint64_t consumed_new_ = 0;
  std::uint64_t superseded_ = 0;
  std::uint64_t drained_ = 0;
  bool shutdown_ = false;
};

}  // namespace mecvr::openxr
