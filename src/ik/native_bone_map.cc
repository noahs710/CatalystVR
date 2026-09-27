#include "ik/native_bone_map.h"

#include <algorithm>

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

}  // namespace mecvr::ik
