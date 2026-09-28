#include <cstring>
#include <iostream>
#include <vector>

#include "ik/faith_skeleton_contract.h"
#include "render/faith_palette_match.h"

namespace {

void PutRowMatrix(std::vector<unsigned char>* bytes, std::size_t index,
                  double x, double y, double z) {
  float matrix[16] = {1.0f, 0.0f, 0.0f, static_cast<float>(x),
                      0.0f, 1.0f, 0.0f, static_cast<float>(y),
                      0.0f, 0.0f, 1.0f, static_cast<float>(z),
                      0.0f, 0.0f, 0.0f, 1.0f};
  std::memcpy(bytes->data() + index * 64, matrix, sizeof(matrix));
}

}  // namespace

int main() {
  constexpr std::size_t kMatrices = 169;
  std::vector<unsigned char> bytes(kMatrices * 64, 0);
  for (std::size_t i = 0; i < mecvr::ik::kFaithArmBones.size(); ++i) {
    const auto& bone = mecvr::ik::kFaithArmBones[i];
    PutRowMatrix(&bytes, bone.index, bone.model_position[0],
                 bone.model_position[1], bone.model_position[2]);
  }

  mecvr::render::BonePaletteCandidate candidate;
  candidate.stride = 64;
  candidate.matrix_count = kMatrices;
  candidate.layout = mecvr::render::BoneMatrixLayout::kAffine4x4RowMajor;
  const auto match =
      mecvr::render::MatchFaithArmPalette(bytes.data(), bytes.size(), candidate);
  if (!match.matched ||
      match.pose_space != mecvr::render::FaithPalettePoseSpace::kModelSpace ||
      match.scale < 0.99 || match.scale > 1.01) {
    std::cerr << "FAIL: retail model-space geometry did not match\n";
    return 1;
  }

  PutRowMatrix(&bytes, mecvr::ik::kFaithArmBones[6].index, 100.0, 100.0,
               100.0);
  const auto rejected =
      mecvr::render::MatchFaithArmPalette(bytes.data(), bytes.size(), candidate);
  if (rejected.matched) {
    std::cerr << "FAIL: asymmetric geometry was accepted\n";
    return 1;
  }
  std::cout << "FAITH_PALETTE_MATCH_TEST: PASS\n";
  return 0;
}
