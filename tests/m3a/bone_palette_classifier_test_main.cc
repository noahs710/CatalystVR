#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

#include "render/bone_palette_classifier.h"

namespace {

bool Check(bool condition, const char* label) {
  if (!condition) std::cerr << "FAIL: " << label << '\n';
  return condition;
}

void Put3x4(std::vector<unsigned char>* bytes, std::size_t offset,
            float translation) {
  const float matrix[12] = {1, 0, 0, translation, 0, 1, 0, translation * 0.5f,
                            0, 0, 1, -translation};
  std::memcpy(bytes->data() + offset, matrix, sizeof(matrix));
}

void Put4x4(std::vector<unsigned char>* bytes, std::size_t offset,
            float angle, float translation) {
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  const float matrix[16] = {c, 0, s, translation, 0, 1, 0, 0,
                            -s, 0, c, -translation, 0, 0, 0, 1};
  std::memcpy(bytes->data() + offset, matrix, sizeof(matrix));
}

}  // namespace

int main() {
  bool ok = true;
  std::vector<unsigned char> compact(32 + 64 * 48, 0xcd);
  for (std::size_t i = 0; i < 64; ++i) Put3x4(&compact, 32 + i * 48, i * 0.1f);
  const auto compact_result =
      mecvr::render::ClassifyBonePalette(compact.data(), compact.size());
  ok &= Check(compact_result.valid(), "3x4 palette detected");
  ok &= Check(compact_result.offset == 32 && compact_result.stride == 48 &&
                  compact_result.matrix_count == 64,
              "3x4 palette geometry");

  std::vector<unsigned char> full(16 + 72 * 64, 0xa5);
  for (std::size_t i = 0; i < 72; ++i)
    Put4x4(&full, 16 + i * 64, static_cast<float>(i) * 0.01f,
           static_cast<float>(i));
  const auto full_result =
      mecvr::render::ClassifyBonePalette(full.data(), full.size());
  ok &= Check(full_result.valid(), "4x4 palette detected");
  ok &= Check(full_result.offset == 16 && full_result.stride == 64 &&
                  full_result.matrix_count == 72,
              "4x4 palette geometry");

  std::vector<unsigned char> single(64, 0);
  Put4x4(&single, 0, 0.0f, 1.0f);
  ok &= Check(!mecvr::render::ClassifyBonePalette(single.data(), single.size())
                   .valid(),
              "single camera matrix rejected");
  std::vector<unsigned char> noise(4096);
  for (std::size_t i = 0; i < noise.size(); ++i)
    noise[i] = static_cast<unsigned char>((i * 73 + 19) & 0xff);
  ok &= Check(!mecvr::render::ClassifyBonePalette(noise.data(), noise.size())
                   .valid(),
              "noise rejected");
  std::cout << (ok ? "BONE_PALETTE_CLASSIFIER_TEST: PASS\n"
                   : "BONE_PALETTE_CLASSIFIER_TEST: FAIL\n");
  return ok ? 0 : 1;
}
