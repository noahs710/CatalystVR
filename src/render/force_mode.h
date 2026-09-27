#pragma once

// Pure display-mode policy for the VR runtime. This module deliberately has
// no DXGI/OpenXR dependency: callers may use it from probes, the launcher, or
// tests without changing hook behavior.

#include <optional>
#include <string>

namespace mecvr::render {

struct DisplayMode {
  int width = 0;
  int height = 0;
  int refresh_hz = 0;
};

enum class DisplayModeSource { kNone, kEnvironment, kRuntime };

struct ForcedMode {
  DisplayMode mode;
  DisplayModeSource source = DisplayModeSource::kNone;
  std::string description;
};

// Parse an environment-style WxH value. "" means unspecified, "0" is the
// explicit kill-switch, and malformed values fail closed.
std::optional<DisplayMode> ParseResolution(const std::string& value);

// Parse a refresh override. Empty means unspecified; zero is a kill-switch
// for the refresh field only; malformed values fail closed.
std::optional<int> ParseRefresh(const std::string& value);

// Resolve explicit environment values over a runtime recommendation. A
// malformed explicit value disables that field rather than guessing. A
// runtime mode is used only when no explicit resolution was supplied.
std::optional<ForcedMode> ResolveForcedMode(
    const std::string& env_resolution, const std::string& env_refresh,
    const std::optional<DisplayMode>& runtime_mode);

}  // namespace mecvr::render
