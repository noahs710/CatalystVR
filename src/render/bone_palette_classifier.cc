#include "render/bone_palette_classifier.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mecvr::render {
namespace {

bool Near(float value, float expected, float tolerance = 1e-3f) {
  return std::fabs(value - expected) <= tolerance;
}

bool PlausibleBasis(const float* m, std::size_t row_stride) {
  double norms[3] = {};
  for (std::size_t row = 0; row < 3; ++row) {
    for (std::size_t column = 0; column < 3; ++column) {
      const double value = m[row * row_stride + column];
      if (!std::isfinite(value) || std::fabs(value) > 16.0) return false;
      norms[row] += value * value;
    }
    if (norms[row] < 0.04 || norms[row] > 16.0) return false;
  }
  for (std::size_t a = 0; a < 3; ++a) {
    for (std::size_t b = a + 1; b < 3; ++b) {
      double dot = 0.0;
      for (std::size_t column = 0; column < 3; ++column) {
        dot += static_cast<double>(m[a * row_stride + column]) *
               m[b * row_stride + column];
      }
      if (std::fabs(dot) > 0.45 * std::sqrt(norms[a] * norms[b])) return false;
    }
  }
  return true;
}

bool FiniteTranslation(const float* m, std::size_t row_stride) {
  for (std::size_t row = 0; row < 3; ++row) {
    const float value = m[row * row_stride + 3];
    if (!std::isfinite(value) || std::fabs(value) > 1000000.0f) return false;
  }
  return true;
}

bool Is3x4(const float* m) {
  return PlausibleBasis(m, 4) && FiniteTranslation(m, 4);
}

BoneMatrixLayout Is4x4(const float* m) {
  if (!PlausibleBasis(m, 4)) return BoneMatrixLayout::kNone;
  const bool row = Near(m[12], 0.0f) && Near(m[13], 0.0f) &&
                   Near(m[14], 0.0f) && Near(m[15], 1.0f) &&
                   FiniteTranslation(m, 4);
  const bool column = Near(m[3], 0.0f) && Near(m[7], 0.0f) &&
                      Near(m[11], 0.0f) && Near(m[15], 1.0f) &&
                      std::isfinite(m[12]) && std::isfinite(m[13]) &&
                      std::isfinite(m[14]);
  if (row) return BoneMatrixLayout::kAffine4x4RowMajor;
  if (column) return BoneMatrixLayout::kAffine4x4ColumnMajor;
  return BoneMatrixLayout::kNone;
}

BonePaletteCandidate Scan(const unsigned char* bytes, std::size_t size,
                          std::size_t stride, BoneMatrixLayout requested,
                          std::size_t minimum) {
  BonePaletteCandidate best;
  for (std::size_t offset = 0; offset + stride <= size; offset += 16) {
    std::size_t count = 0;
    BoneMatrixLayout observed = requested;
    while (offset + (count + 1) * stride <= size) {
      float matrix[16] = {};
      std::memcpy(matrix, bytes + offset + count * stride, stride);
      bool plausible = false;
      if (stride == 48) {
        plausible = Is3x4(matrix);
      } else {
        const BoneMatrixLayout layout = Is4x4(matrix);
        plausible = layout != BoneMatrixLayout::kNone &&
                    (count == 0 || layout == observed);
        if (count == 0) observed = layout;
      }
      if (!plausible) break;
      ++count;
    }
    if (count > best.matrix_count) {
      best.offset = offset;
      best.stride = stride;
      best.matrix_count = count;
      best.layout = observed;
      best.confidence =
          std::min(1.0, static_cast<double>(count) /
                            static_cast<double>(std::max<std::size_t>(minimum, 48)));
    }
  }
  if (best.matrix_count < minimum) return {};
  return best;
}

}  // namespace

BonePaletteCandidate ClassifyBonePalette(const void* data, std::size_t size,
                                         std::size_t minimum_matrices) {
  if (data == nullptr || size < 48 * minimum_matrices || minimum_matrices == 0)
    return {};
  const auto* bytes = static_cast<const unsigned char*>(data);
  const BonePaletteCandidate compact =
      Scan(bytes, size, 48, BoneMatrixLayout::kAffine3x4, minimum_matrices);
  const BonePaletteCandidate full =
      Scan(bytes, size, 64, BoneMatrixLayout::kAffine4x4RowMajor,
           minimum_matrices);
  if (!compact.valid()) return full;
  if (!full.valid()) return compact;
  return compact.matrix_count >= full.matrix_count ? compact : full;
}

}  // namespace mecvr::render
