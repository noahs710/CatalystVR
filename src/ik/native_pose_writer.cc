#include "ik/native_pose_writer.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace mecvr::ik {
namespace {

NativeAffineMatrix MatrixFromPose(const TrackedPose& pose) {
  const auto q = camera::QuatNormalize(pose.orientation);
  const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  return {static_cast<float>(1.0 - 2.0 * (yy + zz)),
          static_cast<float>(2.0 * (xy - wz)),
          static_cast<float>(2.0 * (xz + wy)),
          static_cast<float>(pose.position.x),
          static_cast<float>(2.0 * (xy + wz)),
          static_cast<float>(1.0 - 2.0 * (xx + zz)),
          static_cast<float>(2.0 * (yz - wx)),
          static_cast<float>(pose.position.y),
          static_cast<float>(2.0 * (xz - wy)),
          static_cast<float>(2.0 * (yz + wx)),
          static_cast<float>(1.0 - 2.0 * (xx + yy)),
          static_cast<float>(pose.position.z), 0.0f, 0.0f, 0.0f, 1.0f};
}

bool FinitePose(const TrackedPose& pose) {
  return pose.valid && std::isfinite(pose.position.x) &&
         std::isfinite(pose.position.y) && std::isfinite(pose.position.z) &&
         std::isfinite(pose.orientation.x) &&
         std::isfinite(pose.orientation.y) &&
         std::isfinite(pose.orientation.z) &&
         std::isfinite(pose.orientation.w);
}

bool SameCandidate(const render::BonePaletteCandidate& a,
                   const render::BonePaletteCandidate& b) {
  return a.offset == b.offset && a.stride == b.stride &&
         a.matrix_count == b.matrix_count && a.layout == b.layout;
}

bool AdapterMatches(const NativePoseWriteContext& context) {
  const auto& adapter = context.adapter;
  const auto& observation = context.observation;
  return adapter.verified && adapter.constant_buffer_id != 0 &&
         adapter.constant_buffer_id == observation.constant_buffer_id &&
         adapter.resource_size == observation.resource_size &&
         adapter.content_fingerprint != 0 &&
         observation.content_fingerprint != 0 &&
         SameCandidate(adapter.candidate, observation.candidate);
}

bool FiniteAffine(const NativeAffineMatrix& matrix) {
  for (float value : matrix) {
    if (!std::isfinite(value)) return false;
  }
  constexpr float kEpsilon = 1.0e-4f;
  return std::fabs(matrix[12]) <= kEpsilon &&
         std::fabs(matrix[13]) <= kEpsilon &&
         std::fabs(matrix[14]) <= kEpsilon &&
         std::fabs(matrix[15] - 1.0f) <= kEpsilon;
}

void EncodeMatrix(const NativeAffineMatrix& source,
                  render::BoneMatrixLayout layout, std::uint8_t* destination) {
  if (layout == render::BoneMatrixLayout::kAffine3x4) {
    std::memcpy(destination, source.data(), 12 * sizeof(float));
    return;
  }
  if (layout == render::BoneMatrixLayout::kAffine4x4RowMajor) {
    std::memcpy(destination, source.data(), 16 * sizeof(float));
    return;
  }
  NativeAffineMatrix transposed{};
  for (std::size_t row = 0; row < 4; ++row) {
    for (std::size_t column = 0; column < 4; ++column) {
      transposed[column * 4 + row] = source[row * 4 + column];
    }
  }
  std::memcpy(destination, transposed.data(), 16 * sizeof(float));
}

}  // namespace

bool BuildNativePoseMatrices(const HumanoidPoseFrame& pose,
                             NativePoseMatrices* output) {
  if (output == nullptr || !pose.valid || pose.sequence == 0) return false;
  for (std::size_t i = 0; i < output->size(); ++i) {
    if (!FinitePose(pose.joints[i])) return false;
    (*output)[i] = MatrixFromPose(pose.joints[i]);
  }
  return true;
}

NativePoseWriteStatus RewriteNativePalette(
    const NativeBoneMap& map, const NativePoseWriteContext& context,
    const HumanoidPoseFrame& pose, const NativePoseMatrices& matrices,
    const void* source, std::size_t source_size, void* output,
    std::size_t output_size) {
  if (source == nullptr || output == nullptr || source_size == 0 ||
      output_size < source_size || source == output) {
    return NativePoseWriteStatus::kInvalidArgument;
  }
  std::memcpy(output, source, source_size);

  if (!AdapterMatches(context)) {
    return NativePoseWriteStatus::kUnverifiedAdapter;
  }
  if (!ValidateNativeBoneMap(map, context.observation,
                             context.executable_fingerprint)
           .ready()) {
    return NativePoseWriteStatus::kMapRejected;
  }
  if (!pose.valid || pose.sequence == 0 || context.current_pose_sequence == 0 ||
      pose.sequence > context.current_pose_sequence ||
      context.current_pose_sequence - pose.sequence > context.maximum_pose_lag) {
    return NativePoseWriteStatus::kStalePose;
  }

  const auto& candidate = context.observation.candidate;
  const std::size_t encoded_bytes =
      candidate.layout == render::BoneMatrixLayout::kAffine3x4
          ? 12 * sizeof(float)
          : 16 * sizeof(float);
  if ((candidate.layout != render::BoneMatrixLayout::kAffine3x4 &&
       candidate.layout != render::BoneMatrixLayout::kAffine4x4RowMajor &&
       candidate.layout != render::BoneMatrixLayout::kAffine4x4ColumnMajor) ||
      candidate.stride < encoded_bytes || candidate.offset > source_size) {
    return NativePoseWriteStatus::kOutOfBounds;
  }

  for (std::size_t joint = 0; joint < matrices.size(); ++joint) {
    if (!FiniteAffine(matrices[joint])) {
      return NativePoseWriteStatus::kInvalidMatrix;
    }
    const auto palette_index =
        static_cast<std::size_t>(map.joint_indices[joint]);
    if (palette_index >
        (std::numeric_limits<std::size_t>::max() - candidate.offset) /
            candidate.stride) {
      return NativePoseWriteStatus::kOutOfBounds;
    }
    const std::size_t offset =
        candidate.offset + palette_index * candidate.stride;
    if (offset > source_size || source_size - offset < encoded_bytes) {
      return NativePoseWriteStatus::kOutOfBounds;
    }
  }

  auto* destination = static_cast<std::uint8_t*>(output);
  for (std::size_t joint = 0; joint < matrices.size(); ++joint) {
    const auto palette_index =
        static_cast<std::size_t>(map.joint_indices[joint]);
    const std::size_t offset =
        candidate.offset + palette_index * candidate.stride;
    EncodeMatrix(matrices[joint], candidate.layout, destination + offset);
  }
  return NativePoseWriteStatus::kApplied;
}

}  // namespace mecvr::ik
