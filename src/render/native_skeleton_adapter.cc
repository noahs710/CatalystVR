#include "render/native_skeleton_adapter.h"

#include <algorithm>

namespace mecvr::render {

NativeSkeletonAdapter::NativeSkeletonAdapter(std::size_t minimum_matrices,
                                             std::uint64_t stable_frames)
    : minimum_matrices_(std::max<std::size_t>(minimum_matrices, 12)),
      stable_frames_(std::max<std::uint64_t>(stable_frames, 1)) {}

bool NativeSkeletonAdapter::SameLayout(const BonePaletteCandidate& a,
                                       const BonePaletteCandidate& b) {
  return a.offset == b.offset && a.stride == b.stride &&
         a.layout == b.layout;
}

bool NativeSkeletonAdapter::observe(
    const NativePaletteObservation& observation) {
  ++state_.observations;
  const BonePaletteCandidate& candidate = observation.candidate;
  if (!candidate.valid() || candidate.matrix_count < minimum_matrices_ ||
      observation.present_index == 0 ||
      (last_frame_ != 0 && observation.present_index < last_frame_)) {
    state_.candidate = {};
    state_.stable_observations = 0;
    state_.verified = false;
    last_frame_ = observation.present_index;
    return false;
  }

  // CB/SRV hooks can report the same resource more than once during one
  // present. Treat an identical same-frame observation as a duplicate rather
  // than invalidating an otherwise stable candidate.
  if (last_frame_ != 0 && observation.present_index == last_frame_) {
    const bool duplicate =
        state_.constant_buffer_id == observation.constant_buffer_id &&
        state_.resource_size == observation.resource_size &&
        state_.candidate.valid() && SameLayout(state_.candidate, candidate);
    if (duplicate) return false;
    state_.candidate = {};
    state_.stable_observations = 0;
    state_.verified = false;
    return false;
  }

  // Animated matrices change their contents every frame. Resource identity,
  // byte size, and classified layout are the stable palette contract; the
  // fingerprint remains current-frame evidence only.
  const bool same_source =
      state_.constant_buffer_id == observation.constant_buffer_id &&
      state_.resource_size == observation.resource_size;
  if (state_.candidate.valid() && SameLayout(state_.candidate, candidate) &&
      same_source) {
    ++state_.stable_observations;
    state_.candidate.matrix_count =
        std::max(state_.candidate.matrix_count, candidate.matrix_count);
    state_.candidate.confidence =
        std::max(state_.candidate.confidence, candidate.confidence);
  } else {
    state_.candidate = candidate;
    state_.present_index = observation.present_index;
    state_.constant_buffer_id = observation.constant_buffer_id;
    state_.resource_size = observation.resource_size;
    state_.content_fingerprint = observation.content_fingerprint;
    state_.stable_observations = 1;
    state_.verified = false;
  }
  state_.present_index = observation.present_index;
  last_frame_ = observation.present_index;

  const bool promoted = !state_.verified &&
                        state_.stable_observations >= stable_frames_;
  if (promoted) state_.verified = true;
  return promoted;
}

NativeSkeletonAdapterSnapshot NativeSkeletonAdapter::snapshot() const {
  return state_;
}

void NativeSkeletonAdapter::reset() {
  state_ = {};
  last_frame_ = 0;
}

}  // namespace mecvr::render
