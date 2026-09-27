#pragma once

#include <cstdint>

#include "input/actions.h"

namespace mecvr::input {

struct ComfortConfig {
  TurnMode turn_mode = TurnMode::kSmooth;
  float snap_threshold = 0.65f;
  float snap_degrees = 45.0f;
  std::uint64_t snap_cooldown_ms = 250;
  float smooth_pixels_per_input = 14.0f;
  float snap_pixels_per_degree = 12.0f;
};

struct ComfortTurnResult {
  float mouse_delta = 0.0f;
  bool snapped = false;
};

// Camera comfort is a pure input-stage transform. It never mutates the
// player/world transform and is therefore safe to run before gameplay input
// synthesis.
class ComfortFilter {
 public:
  explicit ComfortFilter(const ComfortConfig& config = {});

  ComfortTurnResult FilterTurn(float horizontal_input,
                               std::uint64_t timestamp_ms);
  void Reset();

 private:
  ComfortConfig config_;
  bool snap_armed_ = true;
  std::uint64_t next_snap_ms_ = 0;
};

}  // namespace mecvr::input
