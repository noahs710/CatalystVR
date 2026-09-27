#include "render/force_mode.h"

#include <cstdio>

namespace {
int failures = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::printf("FAIL: %s\n", message);
    ++failures;
  }
}

}  // namespace

int main() {
  using mecvr::render::DisplayMode;
  using mecvr::render::DisplayModeSource;
  using mecvr::render::ParseRefresh;
  using mecvr::render::ParseResolution;
  using mecvr::render::ResolveForcedMode;

  const auto resolution = ParseResolution("4128x2208");
  Check(resolution && resolution->width == 4128 && resolution->height == 2208,
        "valid resolution parses");
  Check(!ParseResolution("1920"), "missing height rejected");
  Check(!ParseResolution("320x200"), "undersized resolution rejected");
  Check(!ParseResolution("0"), "resolution kill-switch is neutral");
  Check(ParseRefresh("72") && *ParseRefresh("72") == 72,
        "valid refresh parses");
  Check(!ParseRefresh("30"), "undersized refresh rejected");

  const auto env = ResolveForcedMode("4128x2208", "72", std::nullopt);
  Check(env && env->source == DisplayModeSource::kEnvironment &&
            env->mode.width == 4128 && env->mode.refresh_hz == 72,
        "environment wins and carries both fields");

  const auto runtime = ResolveForcedMode(
      "", "", DisplayMode{3664, 1920, 90});
  Check(runtime && runtime->source == DisplayModeSource::kRuntime &&
            runtime->mode.height == 1920 && runtime->mode.refresh_hz == 90,
        "runtime recommendation is used when env is absent");

  const auto env_wins = ResolveForcedMode(
      "4128x2208", "", DisplayMode{3664, 1920, 90});
  Check(env_wins && env_wins->source == DisplayModeSource::kEnvironment &&
            env_wins->mode.width == 4128 && env_wins->mode.refresh_hz == 90,
        "explicit resolution wins while runtime refresh is retained");

  Check(!ResolveForcedMode("0", "0", DisplayMode{3664, 1920, 90}),
        "both kill-switches disable the override");
  Check(!ResolveForcedMode("bad", "", DisplayMode{3664, 1920, 90}),
        "malformed explicit resolution fails closed");
  Check(!ResolveForcedMode("", "bad", DisplayMode{3664, 1920, 90}),
        "malformed explicit refresh fails closed");
  Check(!ResolveForcedMode("", "", DisplayMode{320, 200, 90}),
        "unsafe runtime dimensions fail closed");

  if (failures == 0) std::printf("force_mode: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
