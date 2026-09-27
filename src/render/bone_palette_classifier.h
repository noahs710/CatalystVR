#pragma once

#include <cstddef>
#include <cstdint>

namespace mecvr::render {

enum class BoneMatrixLayout : std::uint8_t {
  kNone,
  kAffine3x4,
  kAffine4x4RowMajor,
  kAffine4x4ColumnMajor,
};

struct BonePaletteCandidate {
  std::size_t offset = 0;
  std::size_t stride = 0;
  std::size_t matrix_count = 0;
  BoneMatrixLayout layout = BoneMatrixLayout::kNone;
  double confidence = 0.0;

  bool valid() const { return matrix_count >= 12 && stride != 0; }
};

BonePaletteCandidate ClassifyBonePalette(const void* data, std::size_t size,
                                         std::size_t minimum_matrices = 12);

}  // namespace mecvr::render
