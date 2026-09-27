#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ik/native_bone_map.h"

namespace mecvr::ik {

using NativeAffineMatrix = std::array<float, 16>;
using NativePoseMatrices =
    std::array<NativeAffineMatrix, static_cast<std::size_t>(BodyJoint::kCount)>;

struct NativePoseWriteContext {
  std::uint64_t executable_fingerprint = 0;
  std::uint64_t current_pose_sequence = 0;
  std::uint64_t maximum_pose_lag = 2;
  render::NativePaletteObservation observation{};
  render::NativeSkeletonAdapterSnapshot adapter{};
};

enum class NativePoseWriteStatus : std::uint8_t {
  kApplied,
  kInvalidArgument,
  kUnverifiedAdapter,
  kMapRejected,
  kStalePose,
  kInvalidMatrix,
  kOutOfBounds,
};

// Converts the canonical mod-owned pose into row-major world-space affine
// matrices with translation in elements 3, 7, and 11.
bool BuildNativePoseMatrices(const HumanoidPoseFrame& pose,
                             NativePoseMatrices* output);

// Copies source to output and rewrites only mapped palette entries. No source
// memory is ever modified. The caller may publish output only when kApplied is
// returned; every rejected request leaves output byte-identical to source.
NativePoseWriteStatus RewriteNativePalette(
    const NativeBoneMap& map, const NativePoseWriteContext& context,
    const HumanoidPoseFrame& pose, const NativePoseMatrices& matrices,
    const void* source, std::size_t source_size, void* output,
    std::size_t output_size);

}  // namespace mecvr::ik
