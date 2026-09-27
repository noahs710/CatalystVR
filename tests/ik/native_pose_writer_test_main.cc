#include "ik/native_pose_writer.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using mecvr::ik::BodyJoint;
using mecvr::ik::HumanoidPoseFrame;
using mecvr::ik::NativeBoneMap;
using mecvr::ik::NativePoseMatrices;
using mecvr::ik::NativePoseWriteContext;
using mecvr::ik::NativePoseWriteStatus;
using mecvr::render::BoneMatrixLayout;

struct Fixture {
  NativeBoneMap map;
  NativePoseWriteContext context;
  HumanoidPoseFrame pose;
  NativePoseMatrices matrices{};
  std::vector<std::uint8_t> source;
  std::vector<std::uint8_t> output;

  Fixture() : source(4096, 0x5a), output(4096, 0) {
    context.executable_fingerprint = 0x12345678;
    context.current_pose_sequence = 101;
    context.maximum_pose_lag = 2;
    auto& observation = context.observation;
    observation.present_index = 400;
    observation.constant_buffer_id = 7;
    observation.resource_size = static_cast<std::uint32_t>(source.size());
    observation.content_fingerprint = 0x88776655;
    observation.candidate.offset = 128;
    observation.candidate.stride = 48;
    observation.candidate.matrix_count = 32;
    observation.candidate.layout = BoneMatrixLayout::kAffine3x4;
    observation.candidate.confidence = 0.99;
    context.adapter.candidate = observation.candidate;
    context.adapter.present_index = observation.present_index;
    context.adapter.constant_buffer_id = observation.constant_buffer_id;
    context.adapter.resource_size = observation.resource_size;
    context.adapter.content_fingerprint = observation.content_fingerprint;
    context.adapter.stable_observations = 6;
    context.adapter.verified = true;

    map.executable_fingerprint = context.executable_fingerprint;
    map.resource_size = observation.resource_size;
    map.palette_offset = static_cast<std::uint32_t>(observation.candidate.offset);
    map.palette_stride = static_cast<std::uint32_t>(observation.candidate.stride);
    map.layout = observation.candidate.layout;
    for (std::size_t i = 0; i < matrices.size(); ++i) {
      map.joint_indices[i] = static_cast<std::int32_t>(i);
      matrices[i] = {1.0f, 0.0f, 0.0f, static_cast<float>(i),
                     0.0f, 1.0f, 0.0f, static_cast<float>(i + 1),
                     0.0f, 0.0f, 1.0f, static_cast<float>(i + 2),
                     0.0f, 0.0f, 0.0f, 1.0f};
    }
    pose.sequence = 100;
    pose.valid = true;
  }

  NativePoseWriteStatus rewrite() {
    return mecvr::ik::RewriteNativePalette(
        map, context, pose, matrices, source.data(), source.size(),
        output.data(), output.size());
  }
};

float ReadFloat(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  float value = 0.0f;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

}  // namespace

int main() {
  Fixture valid;
  assert(valid.rewrite() == NativePoseWriteStatus::kApplied);
  assert(valid.source[128] == 0x5a);
  assert(ReadFloat(valid.output, 128 + 3 * sizeof(float)) == 0.0f);
  assert(ReadFloat(valid.output, 128 + 48 + 3 * sizeof(float)) == 1.0f);
  assert(valid.output[128 + 21 * 48] == 0x5a);

  Fixture unverified;
  unverified.context.adapter.verified = false;
  assert(unverified.rewrite() == NativePoseWriteStatus::kUnverifiedAdapter);
  assert(unverified.output == unverified.source);

  Fixture wrong_executable;
  wrong_executable.context.executable_fingerprint ^= 1;
  assert(wrong_executable.rewrite() == NativePoseWriteStatus::kMapRejected);
  assert(wrong_executable.output == wrong_executable.source);

  Fixture stale;
  stale.context.current_pose_sequence = 104;
  assert(stale.rewrite() == NativePoseWriteStatus::kStalePose);
  assert(stale.output == stale.source);

  Fixture invalid_matrix;
  invalid_matrix.matrices[static_cast<std::size_t>(BodyJoint::kHead)][0] =
      std::numeric_limits<float>::quiet_NaN();
  assert(invalid_matrix.rewrite() == NativePoseWriteStatus::kInvalidMatrix);
  assert(invalid_matrix.output == invalid_matrix.source);

  Fixture out_of_bounds;
  out_of_bounds.context.observation.candidate.offset = 4080;
  out_of_bounds.context.adapter.candidate =
      out_of_bounds.context.observation.candidate;
  out_of_bounds.map.palette_offset = 4080;
  assert(out_of_bounds.rewrite() == NativePoseWriteStatus::kOutOfBounds);
  assert(out_of_bounds.output == out_of_bounds.source);

  return 0;
}
