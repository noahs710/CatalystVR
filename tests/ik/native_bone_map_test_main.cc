#include "ik/native_bone_map.h"

#include <cassert>

int main() {
  using mecvr::ik::BodyJoint;
  using mecvr::ik::NativeBoneMap;
  using mecvr::render::BoneMatrixLayout;
  using mecvr::render::NativePaletteObservation;

  NativePaletteObservation observation;
  observation.constant_buffer_id = 4;
  observation.resource_size = 4096;
  observation.candidate.offset = 128;
  observation.candidate.stride = 48;
  observation.candidate.matrix_count = 32;
  observation.candidate.layout = BoneMatrixLayout::kAffine3x4;

  NativeBoneMap map;
  map.executable_fingerprint = 0x1234;
  map.resource_size = observation.resource_size;
  map.palette_offset = static_cast<std::uint32_t>(observation.candidate.offset);
  map.palette_stride = static_cast<std::uint32_t>(observation.candidate.stride);
  map.layout = observation.candidate.layout;
  for (std::size_t i = 0; i < static_cast<std::size_t>(BodyJoint::kCount);
       ++i) {
    map.joint_indices[i] = static_cast<std::int32_t>(i);
  }
  assert(mecvr::ik::ValidateNativeBoneMap(map, observation, 0x1234).ready());
  assert(!mecvr::ik::ValidateNativeBoneMap(map, observation, 0x9999).ready());

  map.joint_indices[1] = map.joint_indices[0];
  const auto duplicate =
      mecvr::ik::ValidateNativeBoneMap(map, observation, 0x1234);
  assert(!duplicate.ready());
  return 0;
}
