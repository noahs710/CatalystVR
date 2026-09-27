#include "render/force_mode.h"

#include <charconv>
#include <cctype>

namespace mecvr::render {
namespace {

constexpr int kMinDimension = 640;
constexpr int kMaxDimension = 16384;
constexpr int kMinRefresh = 60;
constexpr int kMaxRefresh = 240;

bool ParsePositive(const std::string& text, int* out) {
  if (text.empty() || out == nullptr) return false;
  for (const char c : text) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto result = std::from_chars(begin, end, *out);
  return result.ec == std::errc{} && result.ptr == end && *out > 0;
}

bool InRange(int value, int low, int high) {
  return value >= low && value <= high;
}

}  // namespace

std::optional<DisplayMode> ParseResolution(const std::string& value) {
  if (value.empty() || value == "0") return std::nullopt;
  const std::size_t x = value.find('x');
  if (x == std::string::npos || x == 0 || x + 1 >= value.size() ||
      value.find('x', x + 1) != std::string::npos) {
    return std::nullopt;
  }
  int width = 0;
  int height = 0;
  if (!ParsePositive(value.substr(0, x), &width) ||
      !ParsePositive(value.substr(x + 1), &height) ||
      !InRange(width, kMinDimension, kMaxDimension) ||
      !InRange(height, kMinDimension, kMaxDimension)) {
    return std::nullopt;
  }
  return DisplayMode{width, height, 0};
}

std::optional<int> ParseRefresh(const std::string& value) {
  if (value.empty() || value == "0") return std::nullopt;
  int hz = 0;
  if (!ParsePositive(value, &hz) || !InRange(hz, kMinRefresh, kMaxRefresh)) {
    return std::nullopt;
  }
  return hz;
}

std::optional<ForcedMode> ResolveForcedMode(
    const std::string& env_resolution, const std::string& env_refresh,
    const std::optional<DisplayMode>& runtime_mode) {
  const bool resolution_disabled = env_resolution == "0";
  const bool refresh_disabled = env_refresh == "0";
  const bool has_env_resolution = !env_resolution.empty() &&
                                  !resolution_disabled;
  const bool has_env_refresh = !env_refresh.empty() && !refresh_disabled;

  std::optional<DisplayMode> resolution = ParseResolution(env_resolution);
  if (has_env_resolution && !resolution) return std::nullopt;
  std::optional<int> refresh = ParseRefresh(env_refresh);
  if (has_env_refresh && !refresh) return std::nullopt;

  DisplayMode resolved{};
  DisplayModeSource source = DisplayModeSource::kNone;
  if (resolution) {
    resolved = *resolution;
    source = DisplayModeSource::kEnvironment;
  } else if (!resolution_disabled && runtime_mode &&
             InRange(runtime_mode->width, kMinDimension, kMaxDimension) &&
             InRange(runtime_mode->height, kMinDimension, kMaxDimension)) {
    resolved = *runtime_mode;
    source = DisplayModeSource::kRuntime;
  } else if (!resolution_disabled) {
    return std::nullopt;
  }

  if (refresh) {
    resolved.refresh_hz = *refresh;
    source = DisplayModeSource::kEnvironment;
  } else if (!refresh_disabled && runtime_mode &&
             InRange(runtime_mode->refresh_hz, kMinRefresh, kMaxRefresh)) {
    // An explicit resolution may still be paired with the runtime's current
    // refresh recommendation. This keeps the two fields independently
    // fail-closed and avoids silently discarding a known-safe refresh.
    resolved.refresh_hz = runtime_mode->refresh_hz;
  } else if (!refresh_disabled && resolved.refresh_hz != 0 &&
             !InRange(resolved.refresh_hz, kMinRefresh, kMaxRefresh)) {
    resolved.refresh_hz = 0;
  }

  if (resolved.width == 0 && resolved.height == 0 && resolved.refresh_hz == 0) {
    return std::nullopt;
  }
  ForcedMode out;
  out.mode = resolved;
  out.source = source;
  out.description = "forced " + std::to_string(resolved.width) + "x" +
                    std::to_string(resolved.height);
  if (resolved.refresh_hz != 0) {
    out.description += "@" + std::to_string(resolved.refresh_hz) + "Hz";
  }
  return out;
}

}  // namespace mecvr::render
