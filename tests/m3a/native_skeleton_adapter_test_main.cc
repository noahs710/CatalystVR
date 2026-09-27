#include "render/native_skeleton_adapter.h"

#include <array>
#include <cassert>
#include <cstring>

int main() {
  using mecvr::render::BoneMatrixLayout;
  using mecvr::render::BonePaletteCandidate;
  using mecvr::render::NativeSkeletonAdapter;

  std::array<unsigned char, 64 + 48 * 24> bytes{};
  for (std::size_t i = 0; i < 24; ++i) {
    const float matrix[12] = {1.0f, 0.0f, 0.0f, static_cast<float>(i) * 0.01f,
                              0.0f, 1.0f, 0.0f, 0.0f,
                              0.0f, 0.0f, 1.0f, 0.0f};
    std::memcpy(bytes.data() + 64 + i * 48, matrix, sizeof(matrix));
  }
  BonePaletteCandidate candidate =
      mecvr::render::ClassifyBonePalette(bytes.data(), bytes.size(), 24);
  assert(candidate.valid());
  assert(candidate.offset == 64);
  assert(candidate.stride == 48);
  assert(candidate.matrix_count == 24);
  assert(candidate.layout == BoneMatrixLayout::kAffine3x4);

  NativeSkeletonAdapter adapter(24, 3);
  mecvr::render::NativePaletteObservation observation;
  observation.constant_buffer_id = 7;
  observation.resource_size = 4096;
  observation.content_fingerprint = 0xabc;
  observation.candidate = candidate;
  observation.present_index = 10;
  assert(!adapter.observe(observation));
  observation.present_index = 11;
  assert(!adapter.observe(observation));
  observation.present_index = 12;
  assert(adapter.observe(observation));
  const auto verified = adapter.snapshot();
  assert(verified.verified);
  assert(verified.stable_observations == 3);
  assert(verified.candidate.offset == 64);

  // Animated matrices change their content fingerprint while the resource
  // and palette layout remain stable; verification must survive that change.
  observation.present_index = 13;
  observation.content_fingerprint = 0xdef;
  assert(!adapter.observe(observation));
  assert(adapter.snapshot().verified);

  candidate.offset = 128;
  observation.candidate = candidate;
  observation.present_index = 13;
  assert(!adapter.observe(observation));
  assert(!adapter.snapshot().verified);
  observation.candidate = candidate;
  observation.present_index = 14;
  assert(!adapter.observe(observation));
  observation.present_index = 15;
  assert(!adapter.observe(observation));
  observation.present_index = 16;
  assert(!adapter.observe(observation));
  observation.candidate.offset = 64;
  observation.present_index = 17;
  assert(!adapter.observe(observation));
  observation.present_index = 18;
  assert(!adapter.observe(observation));
  observation.present_index = 19;
  assert(adapter.observe(observation));
  observation.present_index = 19;
  assert(!adapter.observe(observation));
  observation.present_index = 20;
  observation.constant_buffer_id = 8;
  assert(!adapter.observe(observation));
  adapter.reset();
  assert(adapter.snapshot().observations == 0);
  return 0;
}
