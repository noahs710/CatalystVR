#include "render/faith_palette_match.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "ik/faith_skeleton_contract.h"

namespace mecvr::render {
namespace {

constexpr std::size_t kRequiredMatrixCount = 119;
constexpr double kMinScale = 0.01;
constexpr double kMaxScale = 10000.0;
constexpr double kRelativeTolerance = 0.22;
constexpr double kMirrorTolerance = 0.12;

struct Vec3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

double Length(const Vec3& value) {
  return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

Vec3 Subtract(const Vec3& a, const Vec3& b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

bool Finite(const Vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

bool ReadTranslation(const unsigned char* bytes, std::size_t size,
                     const BonePaletteCandidate& candidate,
                     std::size_t index, Vec3* result) {
  if (bytes == nullptr || result == nullptr || candidate.stride == 0 ||
      index >= candidate.matrix_count || index >= kRequiredMatrixCount ||
      index > (static_cast<std::size_t>(-1) - candidate.offset) /
                  candidate.stride) {
    return false;
  }
  const std::size_t offset = candidate.offset + index * candidate.stride;
  const std::size_t bytes_per_matrix =
      candidate.layout == BoneMatrixLayout::kAffine3x4 ? 48 : 64;
  if (offset > size || size - offset < bytes_per_matrix ||
      candidate.stride < bytes_per_matrix) {
    return false;
  }

  float matrix[16] = {};
  std::memcpy(matrix, bytes + offset, bytes_per_matrix);
  switch (candidate.layout) {
    case BoneMatrixLayout::kAffine3x4:
    case BoneMatrixLayout::kAffine4x4RowMajor:
      *result = {matrix[3], matrix[7], matrix[11]};
      break;
    case BoneMatrixLayout::kAffine4x4ColumnMajor:
      *result = {matrix[12], matrix[13], matrix[14]};
      break;
    default:
      return false;
  }
  return Finite(*result) && Length(*result) <= kMaxScale * 100.0;
}

double RelativeError(double observed, double expected, double scale) {
  if (!std::isfinite(observed) || expected <= 0.0 || scale <= 0.0) return 1.0;
  return std::fabs(observed / scale - expected) / expected;
}

double EstimateScale(const std::array<double, 3>& observed,
                     const std::array<double, 3>& expected) {
  double numerator = 0.0;
  double denominator = 0.0;
  for (std::size_t i = 0; i < observed.size(); ++i) {
    numerator += observed[i] * expected[i];
    denominator += expected[i] * expected[i];
  }
  return denominator > 0.0 ? numerator / denominator : 0.0;
}

bool CloseToScale(const std::array<double, 3>& observed,
                  const std::array<double, 3>& expected, double* scale,
                  double* error) {
  const double estimated = EstimateScale(observed, expected);
  if (estimated < kMinScale || estimated > kMaxScale) return false;
  double worst = 0.0;
  for (std::size_t i = 0; i < observed.size(); ++i) {
    worst = std::max(worst,
                     RelativeError(observed[i], expected[i], estimated));
  }
  if (scale != nullptr) *scale = estimated;
  if (error != nullptr) *error = worst;
  return worst <= kRelativeTolerance;
}

}  // namespace

FaithPaletteMatch MatchFaithArmPalette(const void* data, std::size_t size,
                                       const BonePaletteCandidate& candidate) {
  FaithPaletteMatch result;
  if (data == nullptr || !candidate.valid() ||
      candidate.matrix_count < kRequiredMatrixCount) {
    return result;
  }

  std::array<Vec3, 8> positions{};
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < positions.size(); ++i) {
    if (!ReadTranslation(bytes, size, candidate,
                         mecvr::ik::kFaithArmBones[i].index, &positions[i])) {
      return result;
    }
  }

  const std::array<double, 3> expected_lengths = {
      Length(Subtract({mecvr::ik::kFaithArmBones[1].model_position[0],
                       mecvr::ik::kFaithArmBones[1].model_position[1],
                       mecvr::ik::kFaithArmBones[1].model_position[2]},
                      {mecvr::ik::kFaithArmBones[0].model_position[0],
                       mecvr::ik::kFaithArmBones[0].model_position[1],
                       mecvr::ik::kFaithArmBones[0].model_position[2]})),
      Length(Subtract({mecvr::ik::kFaithArmBones[2].model_position[0],
                       mecvr::ik::kFaithArmBones[2].model_position[1],
                       mecvr::ik::kFaithArmBones[2].model_position[2]},
                      {mecvr::ik::kFaithArmBones[1].model_position[0],
                       mecvr::ik::kFaithArmBones[1].model_position[1],
                       mecvr::ik::kFaithArmBones[1].model_position[2]})),
      Length(Subtract({mecvr::ik::kFaithArmBones[3].model_position[0],
                       mecvr::ik::kFaithArmBones[3].model_position[1],
                       mecvr::ik::kFaithArmBones[3].model_position[2]},
                      {mecvr::ik::kFaithArmBones[2].model_position[0],
                       mecvr::ik::kFaithArmBones[2].model_position[1],
                       mecvr::ik::kFaithArmBones[2].model_position[2]}))};

  const std::array<double, 3> model_observed = {
      Length(Subtract(positions[1], positions[0])),
      Length(Subtract(positions[2], positions[1])),
      Length(Subtract(positions[3], positions[2]))};
  const std::array<double, 3> model_right_observed = {
      Length(Subtract(positions[5], positions[4])),
      Length(Subtract(positions[6], positions[5])),
      Length(Subtract(positions[7], positions[6]))};
  const std::array<double, 3> local_observed = {
      Length(positions[1]), Length(positions[2]), Length(positions[3])};
  const std::array<double, 3> local_right_observed = {
      Length(positions[5]), Length(positions[6]), Length(positions[7])};

  const auto match_space = [&](const std::array<double, 3>& left,
                               const std::array<double, 3>& right,
                               FaithPalettePoseSpace space) {
    for (std::size_t i = 0; i < left.size(); ++i) {
      const double denominator = std::max({left[i], right[i], 1e-12});
      if (std::fabs(left[i] - right[i]) / denominator > kMirrorTolerance)
        return FaithPaletteMatch{};
    }
    double left_scale = 0.0;
    double right_scale = 0.0;
    double left_error = 0.0;
    double right_error = 0.0;
    if (!CloseToScale(left, expected_lengths, &left_scale, &left_error) ||
        !CloseToScale(right, expected_lengths, &right_scale, &right_error)) {
      return FaithPaletteMatch{};
    }
    if (std::fabs(left_scale - right_scale) /
            std::max({left_scale, right_scale, 1e-12}) >
        kMirrorTolerance) {
      return FaithPaletteMatch{};
    }
    FaithPaletteMatch match;
    match.matched = true;
    match.pose_space = space;
    match.scale = 0.5 * (left_scale + right_scale);
    match.score = 1.0 - std::max(left_error, right_error);
    return match;
  };

  result = match_space(model_observed, model_right_observed,
                       FaithPalettePoseSpace::kModelSpace);
  if (result) return result;
  return match_space(local_observed, local_right_observed,
                     FaithPalettePoseSpace::kLocalSpace);
}

}  // namespace mecvr::render
