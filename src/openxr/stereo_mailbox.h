#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>

#include "render/stereo_frame.h"

namespace mecvr::openxr {

class StereoMailbox {
 public:
  explicit StereoMailbox(std::size_t capacity = 3) : capacity_(capacity ? capacity : 1) {}

  bool tryPublish(render::StereoFramePtr frame) {
    if (!frame || !render::StereoFrameValid(*frame)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= capacity_) {
      queue_.pop_front();
      ++superseded_;
    }
    queue_.push_back(std::move(frame));
    ++published_;
    return true;
  }

  render::StereoFramePtr consumeNewest() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return {};
    render::StereoFramePtr newest = std::move(queue_.back());
    queue_.clear();
    return newest;
  }

  std::size_t drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t count = queue_.size();
    queue_.clear();
    return count;
  }

  std::size_t depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }
  std::size_t published() const { return published_; }
  std::size_t superseded() const { return superseded_; }

 private:
  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::deque<render::StereoFramePtr> queue_;
  std::size_t published_ = 0;
  std::size_t superseded_ = 0;
};

}  // namespace mecvr::openxr
