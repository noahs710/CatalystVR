#include "input/comfort.h"

#include <cmath>

namespace mecvr::input {

ComfortFilter::ComfortFilter(const ComfortConfig& config) : config_(config) {}

ComfortTurnResult ComfortFilter::FilterTurn(float horizontal_input,
                                             std::uint64_t timestamp_ms) {
  ComfortTurnResult result;
  if (config_.turn_mode == TurnMode::kSmooth) {
    result.mouse_delta = horizontal_input * config_.smooth_pixels_per_input;
    return result;
  }

  const float magnitude = std::fabs(horizontal_input);
  if (magnitude < config_.snap_threshold) {
    snap_armed_ = true;
    return result;
  }
  if (!snap_armed_ || timestamp_ms < next_snap_ms_) return result;

  const float direction = horizontal_input < 0.0f ? -1.0f : 1.0f;
  result.mouse_delta = direction * config_.snap_degrees *
                       config_.snap_pixels_per_degree;
  result.snapped = true;
  snap_armed_ = false;
  next_snap_ms_ = timestamp_ms + config_.snap_cooldown_ms;
  return result;
}

void ComfortFilter::Reset() {
  snap_armed_ = true;
  next_snap_ms_ = 0;
}

}  // namespace mecvr::input
