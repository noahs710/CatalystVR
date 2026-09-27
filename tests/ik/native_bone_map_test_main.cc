#include "ik/native_bone_map.h"

#include <cassert>
#include <cstdio>
#include <fstream>

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

  const char* contract_path = "native_bone_map_test.contract";
  {
    std::ofstream contract(contract_path);
    contract << "version=1\n"
             << "executable_fingerprint=0x1234\n"
             << "resource_size=4096\n"
             << "palette_offset=128\n"
             << "palette_stride=48\n"
             << "layout=affine3x4\n"
             << "joint.root=0\n"
             << "joint.head=4\n"
             << "joint.left_hand=8\n"
             << "joint.right_hand=12\n";
  }
  NativeBoneMap loaded;
  assert(mecvr::ik::LoadNativeBoneMap(contract_path, &loaded));
  assert(loaded.executable_fingerprint == 0x1234);
  assert(loaded.joint_indices[static_cast<std::size_t>(BodyJoint::kHead)] == 4);
  assert(!mecvr::ik::LoadNativeBoneMap("missing-native-bone-map.contract",
                                      &loaded));
  std::remove(contract_path);
  return 0;
}
