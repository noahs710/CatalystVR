#include "ik/native_bone_map.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string_view>

#include "ik/faith_skeleton_contract.h"
#include "render/faith_palette_match.h"

namespace {

std::string Trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

bool ParseUnsigned(const std::string& text, std::uint64_t* value) {
  if (value == nullptr || text.empty()) return false;
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(text, &consumed, 0);
    if (consumed != text.size()) return false;
    *value = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseSigned(const std::string& text, std::int32_t* value) {
  if (value == nullptr || text.empty()) return false;
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stoll(text, &consumed, 0);
    if (consumed != text.size() || parsed < INT32_MIN || parsed > INT32_MAX)
      return false;
    *value = static_cast<std::int32_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseLayout(const std::string& text,
                 mecvr::render::BoneMatrixLayout* layout) {
  if (layout == nullptr) return false;
  if (text == "affine3x4") {
    *layout = mecvr::render::BoneMatrixLayout::kAffine3x4;
    return true;
  }
  if (text == "affine4x4_row_major") {
    *layout = mecvr::render::BoneMatrixLayout::kAffine4x4RowMajor;
    return true;
  }
  if (text == "affine4x4_column_major") {
    *layout = mecvr::render::BoneMatrixLayout::kAffine4x4ColumnMajor;
    return true;
  }
  return false;
}

bool JointIndex(std::string_view name, std::size_t* index) {
  if (index == nullptr) return false;
  constexpr std::string_view kNames[] = {
      "root", "pelvis", "chest", "neck", "head", "left_shoulder",
      "left_elbow", "left_wrist", "left_hand", "right_shoulder",
      "right_elbow", "right_wrist", "right_hand", "left_hip", "left_knee",
      "left_ankle", "left_foot", "right_hip", "right_knee", "right_ankle",
      "right_foot"};
  for (std::size_t i = 0; i < std::size(kNames); ++i) {
    if (name == kNames[i]) {
      *index = i;
      return true;
    }
  }
  return false;
}

}  // namespace

namespace mecvr::ik {

NativeBoneMap::NativeBoneMap() {
  joint_indices.fill(-1);
}

NativeBoneMapValidation ValidateNativeBoneMap(
    const NativeBoneMap& map,
    const render::NativePaletteObservation& observation,
    std::uint64_t executable_fingerprint) {
  NativeBoneMapValidation result;
  result.executable_match = map.executable_fingerprint != 0 &&
                            map.executable_fingerprint == executable_fingerprint;
  result.resource_match = map.resource_size != 0 &&
                          map.resource_size == observation.resource_size &&
                          observation.constant_buffer_id != 0;
  result.layout_match =
      map.palette_offset == observation.candidate.offset &&
      map.palette_stride == observation.candidate.stride &&
      map.layout == observation.candidate.layout && map.palette_stride != 0;

  result.complete = true;
  result.unique = true;
  for (std::size_t i = 0; i < map.joint_indices.size(); ++i) {
    const std::int32_t index = map.joint_indices[i];
    // -1 is an intentional partial-map marker. The native arm milestone
    // rewrites only mapped arm entries and leaves Catalyst's torso/legs
    // animation untouched.
    if (index < 0) continue;
    ++result.mapped_joints;
    if (static_cast<std::size_t>(index) >= observation.candidate.matrix_count) {
      result.complete = false;
      continue;
    }
    for (std::size_t j = i + 1; j < map.joint_indices.size(); ++j) {
      if (map.joint_indices[j] >= 0 && map.joint_indices[j] == index)
        result.unique = false;
    }
  }
  constexpr BodyJoint kArmJoints[] = {
      BodyJoint::kLeftShoulder, BodyJoint::kLeftElbow, BodyJoint::kLeftWrist,
      BodyJoint::kLeftHand, BodyJoint::kRightShoulder, BodyJoint::kRightElbow,
      BodyJoint::kRightWrist, BodyJoint::kRightHand};
  result.arm_complete = true;
  for (const BodyJoint joint : kArmJoints) {
    if (map.joint_indices[static_cast<std::size_t>(joint)] < 0) {
      result.arm_complete = false;
      break;
    }
  }
  if (result.mapped_joints == 0) result.complete = false;
  return result;
}

bool BuildFaithArmNativeBoneMap(
    const render::NativePaletteObservation& observation, const void* data,
    std::size_t data_size, std::uint64_t executable_fingerprint,
    NativeBoneMap* output) {
  if (output == nullptr || data == nullptr || data_size == 0 ||
      executable_fingerprint == 0 || observation.constant_buffer_id == 0 ||
      observation.resource_size == 0 ||
      observation.resource_size != data_size ||
      data_size > std::numeric_limits<std::uint32_t>::max() ||
      !observation.candidate.valid()) {
    return false;
  }

  // Geometry is the semantic proof. A classifier result alone only says that
  // bytes look matrix-like and is not enough to arm a title-specific writer.
  if (!render::MatchFaithArmPalette(data, data_size, observation.candidate))
    return false;

  NativeBoneMap parsed;
  parsed.executable_fingerprint = executable_fingerprint;
  parsed.resource_size = observation.resource_size;
  if (observation.candidate.offset >
          std::numeric_limits<std::uint32_t>::max() ||
      observation.candidate.stride >
          std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  parsed.palette_offset =
      static_cast<std::uint32_t>(observation.candidate.offset);
  parsed.palette_stride =
      static_cast<std::uint32_t>(observation.candidate.stride);
  parsed.layout = observation.candidate.layout;

  constexpr BodyJoint kBodyArmJoints[] = {
      BodyJoint::kLeftShoulder, BodyJoint::kLeftElbow,
      BodyJoint::kLeftWrist,    BodyJoint::kLeftHand,
      BodyJoint::kRightShoulder, BodyJoint::kRightElbow,
      BodyJoint::kRightWrist,    BodyJoint::kRightHand};
  for (std::size_t i = 0; i < std::size(kBodyArmJoints); ++i) {
    parsed.joint_indices[static_cast<std::size_t>(kBodyArmJoints[i])] =
        static_cast<std::int32_t>(kFaithArmBones[i].index);
  }

  const auto validation = ValidateNativeBoneMap(
      parsed, observation, executable_fingerprint);
  if (!validation.ready()) return false;
  *output = parsed;
  return true;
}

bool WriteNativeBoneMap(const std::string& path, const NativeBoneMap& map) {
  if (path.empty() || map.executable_fingerprint == 0 ||
      map.resource_size == 0 || map.palette_stride == 0 ||
      map.layout == render::BoneMatrixLayout::kNone) {
    return false;
  }
  std::ofstream stream(path, std::ios::trunc);
  if (!stream.is_open()) return false;
  stream << "# MECVR native Faith arm contract; review before enabling\n"
         << "version=1\n"
         << "executable_fingerprint=0x" << std::hex
         << map.executable_fingerprint << std::dec << "\n"
         << "resource_size=" << map.resource_size << "\n"
         << "palette_offset=" << map.palette_offset << "\n"
         << "palette_stride=" << map.palette_stride << "\n";
  const char* layout = nullptr;
  switch (map.layout) {
    case render::BoneMatrixLayout::kAffine3x4:
      layout = "affine3x4";
      break;
    case render::BoneMatrixLayout::kAffine4x4RowMajor:
      layout = "affine4x4_row_major";
      break;
    case render::BoneMatrixLayout::kAffine4x4ColumnMajor:
      layout = "affine4x4_column_major";
      break;
    default:
      return false;
  }
  stream << "layout=" << layout << "\n";
  constexpr std::array<std::pair<const char*, BodyJoint>, 21> kJointNames{{
      {"root", BodyJoint::kRoot},
      {"pelvis", BodyJoint::kPelvis},
      {"chest", BodyJoint::kChest},
      {"neck", BodyJoint::kNeck},
      {"head", BodyJoint::kHead},
      {"left_shoulder", BodyJoint::kLeftShoulder},
      {"left_elbow", BodyJoint::kLeftElbow},
      {"left_wrist", BodyJoint::kLeftWrist},
      {"left_hand", BodyJoint::kLeftHand},
      {"right_shoulder", BodyJoint::kRightShoulder},
      {"right_elbow", BodyJoint::kRightElbow},
      {"right_wrist", BodyJoint::kRightWrist},
      {"right_hand", BodyJoint::kRightHand},
      {"left_hip", BodyJoint::kLeftHip},
      {"left_knee", BodyJoint::kLeftKnee},
      {"left_ankle", BodyJoint::kLeftAnkle},
      {"left_foot", BodyJoint::kLeftFoot},
      {"right_hip", BodyJoint::kRightHip},
      {"right_knee", BodyJoint::kRightKnee},
      {"right_ankle", BodyJoint::kRightAnkle},
      {"right_foot", BodyJoint::kRightFoot}}};
  for (const auto& [name, joint] : kJointNames) {
    const auto value = map.joint_indices[static_cast<std::size_t>(joint)];
    if (value >= 0) stream << "joint." << name << "=" << value << "\n";
  }
  return stream.good();
}

bool LoadNativeBoneMap(const std::string& path, NativeBoneMap* output) {
  if (path.empty() || output == nullptr) return false;
  std::ifstream stream(path);
  if (!stream.is_open()) return false;

  NativeBoneMap parsed;
  bool saw_version = false;
  std::string line;
  while (std::getline(stream, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) return false;
    const std::string key = Trim(line.substr(0, equals));
    const std::string value = Trim(line.substr(equals + 1));
    if (key == "version") {
      std::uint64_t version = 0;
      if (!ParseUnsigned(value, &version) || version != 1) return false;
      saw_version = true;
    } else if (key == "executable_fingerprint") {
      if (!ParseUnsigned(value, &parsed.executable_fingerprint)) return false;
    } else if (key == "resource_size") {
      std::uint64_t value_u64 = 0;
      if (!ParseUnsigned(value, &value_u64) || value_u64 > UINT32_MAX) return false;
      parsed.resource_size = static_cast<std::uint32_t>(value_u64);
    } else if (key == "palette_offset") {
      std::uint64_t value_u64 = 0;
      if (!ParseUnsigned(value, &value_u64) || value_u64 > UINT32_MAX) return false;
      parsed.palette_offset = static_cast<std::uint32_t>(value_u64);
    } else if (key == "palette_stride") {
      std::uint64_t value_u64 = 0;
      if (!ParseUnsigned(value, &value_u64) || value_u64 > UINT32_MAX) return false;
      parsed.palette_stride = static_cast<std::uint32_t>(value_u64);
    } else if (key == "layout") {
      if (!ParseLayout(value, &parsed.layout)) return false;
    } else if (key.rfind("joint.", 0) == 0) {
      std::size_t joint = 0;
      if (!JointIndex(std::string_view(key).substr(6), &joint) ||
          !ParseSigned(value, &parsed.joint_indices[joint])) {
        return false;
      }
    } else {
      return false;
    }
  }
  if (!saw_version) return false;
  *output = parsed;
  return true;
}

bool FingerprintExecutableFile(const std::string& path,
                               std::uint64_t* fingerprint) {
  if (path.empty() || fingerprint == nullptr) return false;
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) return false;

  constexpr std::uint64_t kOffset = 14695981039346656037ull;
  constexpr std::uint64_t kPrime = 1099511628211ull;
  std::uint64_t hash = kOffset;
  std::array<char, 64 * 1024> block{};
  std::uint64_t bytes = 0;
  while (stream.good()) {
    stream.read(block.data(), static_cast<std::streamsize>(block.size()));
    const std::streamsize count = stream.gcount();
    for (std::streamsize i = 0; i < count; ++i) {
      hash ^= static_cast<unsigned char>(block[static_cast<std::size_t>(i)]);
      hash *= kPrime;
    }
    bytes += static_cast<std::uint64_t>(count);
  }
  if (stream.bad() || bytes == 0) return false;
  *fingerprint = hash;
  return true;
}

}  // namespace mecvr::ik
