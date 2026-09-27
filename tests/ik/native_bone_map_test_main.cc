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
  map.joint_indices.fill(-1);
  map.joint_indices[static_cast<std::size_t>(BodyJoint::kRoot)] = 0;
  map.joint_indices[static_cast<std::size_t>(BodyJoint::kLeftShoulder)] = 5;
  map.joint_indices[static_cast<std::size_t>(BodyJoint::kLeftElbow)] = 6;
  map.joint_indices[static_cast<std::size_t>(BodyJoint::kLeftWrist)] = 7;
  map.joint_indices[static_cast<std::size_t>(BodyJoint::kLeftHand)] = 8;
  map.joint_indices[static_cast<std::size_t>(BodyJoint::kRightShoulder)] = 9;
  map.joint_indices[static_cast<std::size_t>(BodyJoint::kRightElbow)] = 10;
  map.joint_indices[static_cast<std::size_t>(BodyJoint::kRightWrist)] = 11;
  map.joint_indices[static_cast<std::size_t>(BodyJoint::kRightHand)] = 12;
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
             << "joint.left_shoulder=5\n"
             << "joint.left_elbow=6\n"
             << "joint.left_wrist=7\n"
             << "joint.left_hand=8\n"
             << "joint.right_shoulder=9\n"
             << "joint.right_elbow=10\n"
             << "joint.right_wrist=11\n"
             << "joint.right_hand=12\n";
  }
  NativeBoneMap loaded;
  assert(mecvr::ik::LoadNativeBoneMap(contract_path, &loaded));
  assert(loaded.executable_fingerprint == 0x1234);
  assert(loaded.joint_indices[static_cast<std::size_t>(BodyJoint::kLeftElbow)] == 6);
  assert(!mecvr::ik::LoadNativeBoneMap("missing-native-bone-map.contract",
                                      &loaded));
  std::remove(contract_path);

  const char* executable_path = "native_bone_map_test.image";
  {
    std::ofstream image(executable_path, std::ios::binary);
    image << "CatalystVR\n";
  }
  std::uint64_t first_fingerprint = 0;
  std::uint64_t repeated_fingerprint = 0;
  if (!mecvr::ik::FingerprintExecutableFile(executable_path,
                                             &first_fingerprint) ||
      !mecvr::ik::FingerprintExecutableFile(executable_path,
                                             &repeated_fingerprint) ||
      first_fingerprint == 0 || first_fingerprint != repeated_fingerprint) {
    return 1;
  }
  {
    std::ofstream image(executable_path, std::ios::binary | std::ios::app);
    image << "changed";
  }
  std::uint64_t changed_fingerprint = 0;
  if (!mecvr::ik::FingerprintExecutableFile(executable_path,
                                             &changed_fingerprint) ||
      changed_fingerprint == first_fingerprint ||
      mecvr::ik::FingerprintExecutableFile("missing-executable.image",
                                            &changed_fingerprint)) {
    return 1;
  }
  std::remove(executable_path);
  return 0;
}
