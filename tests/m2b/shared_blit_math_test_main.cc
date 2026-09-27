#include <cmath>
#include <iostream>

#include "render/shared_blit_math.h"

namespace {

bool Near(float a, float b) { return std::fabs(a - b) < 0.0001f; }

bool Check(bool value, const char* label) {
  if (!value) std::cerr << "FAIL: " << label << "\n";
  return value;
}

}  // namespace

int main() {
  using mecvr::render::CenterCropUv;
  bool ok = true;

  const auto square = CenterCropUv(1920, 1080, 1000, 1000);
  ok &= Check(square.valid, "16:9 to square valid");
  ok &= Check(Near(square.origin_x, 0.21875f) &&
                  Near(square.origin_y, 0.0f) &&
                  Near(square.extent_x, 0.5625f) &&
                  Near(square.extent_y, 1.0f),
              "16:9 to square center crops horizontally");

  const auto wide = CenterCropUv(2560, 1080, 1920, 1080);
  ok &= Check(wide.valid && Near(wide.origin_x, 0.125f) &&
                  Near(wide.extent_x, 0.75f) && Near(wide.extent_y, 1.0f),
              "21:9 to 16:9 center crops horizontally");

  const auto portrait = CenterCropUv(1080, 1920, 1200, 1600);
  ok &= Check(portrait.valid && Near(portrait.origin_x, 0.0f) &&
                  square.origin_x + square.extent_x <= 1.0f &&
                  portrait.origin_y >= 0.0f &&
                  portrait.origin_y + portrait.extent_y <= 1.0f,
              "portrait crop remains in range");

  const auto exact = CenterCropUv(1832, 1920, 1832, 1920);
  ok &= Check(exact.valid && Near(exact.origin_x, 0.0f) &&
                  Near(exact.origin_y, 0.0f) && Near(exact.extent_x, 1.0f) &&
                  Near(exact.extent_y, 1.0f),
              "equal aspect uses complete source");
  ok &= Check(!CenterCropUv(0, 1080, 1000, 1000).valid,
              "zero source rejected");
  ok &= Check(!CenterCropUv(1920, 1080, 0, 1000).valid,
              "zero target rejected");

  std::cout << (ok ? "SHARED_BLIT_MATH_TEST: PASS\n"
                   : "SHARED_BLIT_MATH_TEST: FAIL\n");
  return ok ? 0 : 1;
}
