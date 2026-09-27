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

NativePaletteTargetTracker::NativePaletteTargetTracker(
    std::uint64_t stable_frames)
    : stable_frames_(std::max<std::uint64_t>(stable_frames, 1)) {}

void NativePaletteTargetTracker::observePalette(
    std::uint32_t resource_id, const BonePaletteCandidate& candidate,
    std::uint64_t present_index) {
  if (resource_id == 0 || present_index == 0 || !candidate.valid()) return;
  // Once a target has repeatable draw evidence, unrelated animated palettes
  // must not demote it. A future resource-lifetime generation can explicitly
  // call reset() when the underlying D3D resource is destroyed/reused.
  if (state_.verified && state_.resource_id != resource_id) return;
  if (state_.resource_id != resource_id || !candidate_.valid() ||
      candidate_.offset != candidate.offset ||
      candidate_.stride != candidate.stride ||
      candidate_.layout != candidate.layout) {
    reset();
    state_.resource_id = resource_id;
    candidate_ = candidate;
  }
  if (present_index <= last_palette_present_) return;
  last_palette_present_ = present_index;
  state_.present_index = present_index;
  ++state_.palette_frames;
  state_.verified = state_.palette_frames >= stable_frames_ &&
                    (state_.draw_bound_frames >= stable_frames_ ||
                     state_.vertex_bound_frames >= stable_frames_);
}

void NativePaletteTargetTracker::noteDraw(
    std::uint32_t resource_id, std::uint64_t present_index,
    std::uint64_t draw_index, std::uintptr_t vertex_shader,
    std::uintptr_t pixel_shader) {
  if (resource_id == 0 || present_index == 0 ||
      state_.resource_id != resource_id || !candidate_.valid()) return;
  if (present_index == last_draw_present_) return;
  last_draw_present_ = present_index;
  state_.draw_index = draw_index;
  state_.vertex_shader = vertex_shader;
  state_.pixel_shader = pixel_shader;
  ++state_.draw_bound_frames;
  state_.verified = state_.palette_frames >= stable_frames_ &&
                    (state_.draw_bound_frames >= stable_frames_ ||
                     state_.vertex_bound_frames >= stable_frames_);
}

void NativePaletteTargetTracker::noteVertexBinding(
    std::uint32_t resource_id, std::uint64_t present_index,
    std::uintptr_t vertex_shader, std::uintptr_t pixel_shader) {
  if (resource_id == 0 || present_index == 0 || vertex_shader == 0 ||
      state_.resource_id != resource_id || !candidate_.valid()) {
    return;
  }
  if (present_index == last_draw_present_) return;
  last_draw_present_ = present_index;
  state_.vertex_shader = vertex_shader;
  state_.pixel_shader = pixel_shader;
  ++state_.vertex_bound_frames;
  state_.verified = state_.palette_frames >= stable_frames_ &&
                    (state_.draw_bound_frames >= stable_frames_ ||
                     state_.vertex_bound_frames >= stable_frames_);
}

void NativePaletteTargetTracker::reset() {
  state_ = {};
  candidate_ = {};
  last_palette_present_ = 0;
  last_draw_present_ = 0;
}

}  // namespace mecvr::render
