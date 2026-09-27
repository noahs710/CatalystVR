#include "ik/native_bone_map.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string_view>

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
    if (index < 0 || static_cast<std::size_t>(index) >=
                         observation.candidate.matrix_count) {
      result.complete = false;
      continue;
    }
    for (std::size_t j = i + 1; j < map.joint_indices.size(); ++j) {
      if (map.joint_indices[j] == index) result.unique = false;
    }
  }
  return result;
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

}  // namespace mecvr::ik
