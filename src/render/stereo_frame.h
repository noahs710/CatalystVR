#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace mecvr::render {

// A pair of eye images produced from one game simulation epoch and one pose
// snapshot. The worker must reject incomplete or cross-epoch pairs.
struct StereoFrame {
  std::uint64_t epoch = 0;
  std::uint64_t pose_sequence = 0;
  std::uint32_t width[2] = {};
  std::uint32_t height[2] = {};
  std::int64_t capture_time_ns = 0;
  std::vector<std::uint8_t> pixels_rgba[2];
};

using StereoFramePtr = std::shared_ptr<const StereoFrame>;

// Proof carried across both native eye renders. A pair is submit-safe only when
// the game scene, prepared render state and predicted XR pose remain identical.
struct M6EpochToken {
  std::uint64_t simulation_generation = 0;
  std::uint64_t scene_generation = 0;
  std::uint64_t prepared_scene_identity = 0;
  std::uint64_t pose_sequence = 0;
  std::int64_t predicted_display_time = 0;
  std::uint64_t space_generation = 0;
};

inline bool SameM6Epoch(const M6EpochToken& a, const M6EpochToken& b) {
  return a.simulation_generation == b.simulation_generation &&
         a.scene_generation == b.scene_generation &&
         a.prepared_scene_identity == b.prepared_scene_identity &&
         a.pose_sequence == b.pose_sequence &&
         a.predicted_display_time == b.predicted_display_time &&
         a.space_generation == b.space_generation;
}

class M6EpochGate {
 public:
  bool beginPair(const M6EpochToken& token) {
    if (state_ != State::kIdle || !Valid(token)) return Reject();
    token_ = token;
    state_ = State::kLeftBegun;
    return true;
  }

  bool commitLeft(const M6EpochToken& token, bool history_mutated = false) {
    if (state_ != State::kLeftBegun || history_mutated || !Matches(token))
      return Reject();
    state_ = State::kLeftCommitted;
    return true;
  }

  bool beginRight(const M6EpochToken& token) {
    if (state_ != State::kLeftCommitted || !Matches(token)) return Reject();
    state_ = State::kRightBegun;
    return true;
  }

  bool commitRight(const M6EpochToken& token, bool history_mutated = false) {
    if (state_ != State::kRightBegun || history_mutated || !Matches(token))
      return Reject();
    state_ = State::kComplete;
    return true;
  }

  bool canSubmit() const { return state_ == State::kComplete; }

 private:
  enum class State { kIdle, kLeftBegun, kLeftCommitted, kRightBegun,
                     kComplete, kRejected };

  static bool Valid(const M6EpochToken& token) {
    return token.simulation_generation != 0 && token.scene_generation != 0 &&
           token.prepared_scene_identity != 0 && token.pose_sequence != 0 &&
           token.predicted_display_time != 0 && token.space_generation != 0;
  }
  bool Matches(const M6EpochToken& token) const {
    return SameM6Epoch(token_, token);
  }
  bool Reject() {
    state_ = State::kRejected;
    return false;
  }

  M6EpochToken token_{};
  State state_ = State::kIdle;
};

inline bool StereoFrameValid(const StereoFrame& frame) {
  if (frame.width[0] != frame.width[1] ||
      frame.height[0] != frame.height[1]) {
    return false;
  }
  for (int eye = 0; eye < 2; ++eye) {
    if (frame.width[eye] == 0 || frame.height[eye] == 0 ||
        frame.pixels_rgba[eye].empty()) {
      return false;
    }
    const std::uint64_t expected =
        static_cast<std::uint64_t>(frame.width[eye]) * frame.height[eye] * 4u;
    if (expected != frame.pixels_rgba[eye].size()) return false;
  }
  return frame.epoch != 0 && frame.pose_sequence != 0;
}

inline bool SameStereoEpoch(const StereoFrame& frame,
                            std::uint64_t epoch,
                            std::uint64_t pose_sequence) {
  return StereoFrameValid(frame) && frame.epoch == epoch &&
         frame.pose_sequence == pose_sequence;
}

}  // namespace mecvr::render
