// M2B mono frame implementation (plan T11, render stream). See m2b_mono.h.

#include "render/m2b_mono.h"

#include <chrono>

namespace mecvr::render {
namespace {

// STOP S3: compile-time proof this translation unit defines no camera,
// stereo, or gameplay path — the attestation constant must stay false.
static_assert(!kM2bTouchesGameCamera,
              "M2B mono module must never touch the game camera");

}  // namespace

std::int64_t SteadyNanos() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

MonoFramePtr MakeSyntheticFrame(std::uint64_t sequence, std::uint32_t width,
                                std::uint32_t height, std::uint32_t seed) {
  auto frame = std::make_shared<MonoFrame>();
  frame->sequence = sequence;
  frame->capture_time_ns = SteadyNanos();
  frame->width = width;
  frame->height = height;
  const std::size_t count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  frame->pixels_rgba.resize(count * 4u);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::size_t i =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) *
          4u;
      frame->pixels_rgba[i + 0] =
          static_cast<std::uint8_t>((x + seed) & 0xffu);
      frame->pixels_rgba[i + 1] =
          static_cast<std::uint8_t>((y + (seed >> 8)) & 0xffu);
      frame->pixels_rgba[i + 2] =
          static_cast<std::uint8_t>(((x ^ y) + (seed >> 16)) & 0xffu);
      frame->pixels_rgba[i + 3] = 255u;
    }
  }
  // Embed the sequence LE in the first 8 bytes so content-distinctness
  // follows directly from sequence-distinctness.
  for (int b = 0; b < 8 && static_cast<std::size_t>(b) < frame->pixels_rgba.size();
       ++b) {
    frame->pixels_rgba[static_cast<std::size_t>(b)] =
        static_cast<std::uint8_t>((sequence >> (b * 8)) & 0xffu);
  }
  return frame;
}

bool FramesPixelIdentical(const MonoFrame& a, const MonoFrame& b) {
  return a.width == b.width && a.height == b.height &&
         a.pixels_rgba == b.pixels_rgba;
}

}  // namespace mecvr::render
