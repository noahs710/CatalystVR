#include "render/stereo_eye.h"

#include <cstdlib>
#include <iostream>

namespace {

void Check(bool ok, const char* message) {
  if (!ok) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  }
}

}  // namespace

int main() {
  using mecvr::render::ClassifyStereoViewport;
  using mecvr::render::StereoEye;

  Check(ClassifyStereoViewport({0.0, 960.0}, 1920.0) == StereoEye::kLeft,
        "left half classified");
  Check(ClassifyStereoViewport({960.0, 960.0}, 1920.0) == StereoEye::kRight,
        "right half classified");
  Check(ClassifyStereoViewport({0.0, 1920.0}, 1920.0) == StereoEye::kUnknown,
        "full frame is not guessed as an eye");
  Check(ClassifyStereoViewport({320.0, 640.0}, 1920.0) == StereoEye::kUnknown,
        "arbitrary sub-rectangle is rejected");
  Check(ClassifyStereoViewport({0.0, 960.0}, 0.0) == StereoEye::kUnknown,
        "zero target is rejected");
  std::cout << "stereo eye classifier ok\n";
  return 0;
}
