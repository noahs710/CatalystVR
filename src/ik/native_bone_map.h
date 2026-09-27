#pragma once

#include <array>
#include <cstdint>

#include "ik/full_body_ik.h"
#include "render/native_skeleton_adapter.h"

namespace mecvr::ik {

// Versioned, title-specific evidence required before a canonical pose can be
// considered compatible with a retail skeleton. This is metadata only: it is
// never used to write a Catalyst buffer by itself.
struct NativeBoneMap {
  std::uint64_t executable_fingerprint = 0;
  std::uint32_t resource_size = 0;
  std::uint32_t palette_offset = 0;
  std::uint32_t palette_stride = 0;
  render::BoneMatrixLayout layout = render::BoneMatrixLayout::kNone;
  std::array<std::int32_t, static_cast<std::size_t>(BodyJoint::kCount)>
      joint_indices{};

  NativeBoneMap();
};

struct NativeBoneMapValidation {
  bool executable_match = false;
  bool resource_match = false;
  bool layout_match = false;
  bool complete = false;
  bool unique = false;

  bool ready() const {
    return executable_match && resource_match && layout_match && complete &&
           unique;
  }
};

NativeBoneMapValidation ValidateNativeBoneMap(
    const NativeBoneMap& map,
    const render::NativePaletteObservation& observation,
    std::uint64_t executable_fingerprint);

}  // namespace mecvr::ik
