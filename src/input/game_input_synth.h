#pragma once

#include <cstdint>

#include "input/motion_scheme.h"
#include "input/comfort.h"
#include "ik/parkour_intent.h"
#include "openxr/xr_backend.h"

namespace mecvr::input {

struct ParkourKeyConfig {
  std::uint16_t vault_scan = 0x39;  // Space
  std::uint16_t climb_scan = 0x12;  // E
  std::uint16_t slide_scan = 0x1d;  // Left Ctrl
};

// Optional, foreground-gated desktop input bridge. It turns the device-
// neutral motion scheme into ordinary Catalyst keyboard/mouse events while
// keeping the game process unaware of OpenXR. Disabled unless explicitly
// enabled by MECVR_ENABLE_INPUT=1.
class GameInputSynth {
 public:
  explicit GameInputSynth(std::uint32_t target_pid,
                          const ComfortConfig& comfort = {},
                          ParkourKeyConfig parkour_keys = {});
  ~GameInputSynth();

  void update(std::uint64_t timestamp_ms,
              const openxr::ControllerState& left,
              const openxr::ControllerState& right,
              bool physically_crouched = false,
              bool physical_jump = false,
              const ik::ParkourIntents& parkour = {});
  void releaseAll();

 private:
  void setKey(std::uint16_t scan_code, bool down);
  void tapKey(std::uint16_t scan_code);
  void setMouseButton(std::uint32_t flag, bool down);
  bool foreground() const;
  bool tryFocusTarget();

  std::uint32_t target_pid_ = 0;
  MotionScheme scheme_;
  ComfortFilter comfort_;
  ParkourKeyConfig parkour_keys_{};
  bool key_state_[6] = {};
  bool mouse_left_ = false;
  bool mouse_right_ = false;
  bool previous_jump_ = false;
  bool previous_melee_ = false;
  bool parkour_climb_ = false;
  std::uint64_t last_focus_attempt_ms_ = 0;
};

}  // namespace mecvr::input
