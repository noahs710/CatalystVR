#include "input/game_input_synth.h"

#include <windows.h>

#include <variant>

namespace mecvr::input {
namespace {

constexpr std::uint16_t kW = 0x11;
constexpr std::uint16_t kA = 0x1e;
constexpr std::uint16_t kS = 0x1f;
constexpr std::uint16_t kD = 0x20;
constexpr std::uint16_t kShift = 0x2a;

PoseState ToPose(const openxr::XrPosef& pose, bool valid) {
  PoseState out;
  out.valid = valid;
  out.quality = valid ? 1.0f : 0.0f;
  out.position[0] = pose.position.x;
  out.position[1] = pose.position.y;
  out.position[2] = pose.position.z;
  out.orientation[0] = pose.orientation.x;
  out.orientation[1] = pose.orientation.y;
  out.orientation[2] = pose.orientation.z;
  out.orientation[3] = pose.orientation.w;
  return out;
}

struct TargetWindowSearch {
  DWORD pid = 0;
  HWND window = nullptr;
};

BOOL CALLBACK FindTargetWindow(HWND window, LPARAM parameter) {
  auto* search = reinterpret_cast<TargetWindowSearch*>(parameter);
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid == search->pid && IsWindowVisible(window) &&
      GetWindow(window, GW_OWNER) == nullptr) {
    search->window = window;
    return FALSE;
  }
  return TRUE;
}

}  // namespace

GameInputSynth::GameInputSynth(std::uint32_t target_pid,
                               const ComfortConfig& comfort,
                               ParkourKeyConfig parkour_keys)
    : target_pid_(target_pid), comfort_(comfort), parkour_keys_(parkour_keys) {}

GameInputSynth::~GameInputSynth() { releaseAll(); }

bool GameInputSynth::foreground() const {
  HWND window = GetForegroundWindow();
  if (window == nullptr) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  return pid == target_pid_;
}

bool GameInputSynth::tryFocusTarget() {
  TargetWindowSearch search{target_pid_, nullptr};
  EnumWindows(&FindTargetWindow, reinterpret_cast<LPARAM>(&search));
  if (search.window == nullptr) return false;
  if (IsIconic(search.window)) ShowWindow(search.window, SW_RESTORE);
  // Explicit MECVR input enablement authorizes the game window to receive
  // the synthesized desktop route. This is required when a VR compositor or
  // runtime monitor owns foreground focus, otherwise SendInput is delivered
  // to that monitor and locomotion appears to do nothing in Catalyst.
  SetForegroundWindow(search.window);
  return foreground();
}

void GameInputSynth::setKey(std::uint16_t scan_code, bool down) {
  INPUT event{};
  event.type = INPUT_KEYBOARD;
  event.ki.wScan = scan_code;
  event.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0u : KEYEVENTF_KEYUP);
  (void)SendInput(1, &event, sizeof(event));
}

void GameInputSynth::tapKey(std::uint16_t scan_code) {
  setKey(scan_code, true);
  setKey(scan_code, false);
}

void GameInputSynth::setMouseButton(std::uint32_t flag, bool down) {
  INPUT event{};
  event.type = INPUT_MOUSE;
  event.mi.dwFlags = down ? flag : (flag << 1);
  (void)SendInput(1, &event, sizeof(event));
}

void GameInputSynth::update(std::uint64_t timestamp_ms,
                            const openxr::ControllerState& left,
                            const openxr::ControllerState& right,
                            bool physically_crouched,
                            bool physical_jump,
                            const ik::ParkourIntents& parkour,
                            const PoseState& head) {
  MotionSample sample;
  sample.timestamp = timestamp_ms;
  sample.head = head;
  sample.left_stick = {left.thumbstick_x, left.thumbstick_y};
  sample.right_stick = {right.thumbstick_x, right.thumbstick_y};
  sample.left_hand = ToPose(left.grip_pose, left.pose_valid);
  sample.right_hand = ToPose(right.grip_pose, right.pose_valid);
  // Some runtimes expose the analog action without setting a separate
  // digital button bit. Preserve both paths so trigger shooting and grip
  // locomotion remain reliable across controllers and OpenXR runtimes.
  sample.left_trigger = left.trigger_value > 0.55f ||
                        (left.buttons & openxr::kButtonTrigger) != 0;
  sample.right_trigger = right.trigger_value > 0.55f ||
                         (right.buttons & openxr::kButtonTrigger) != 0;
  sample.left_grip = left.squeeze_value > 0.55f ||
                     (left.buttons & openxr::kButtonSqueeze) != 0;
  sample.right_grip = right.squeeze_value > 0.55f ||
                      (right.buttons & openxr::kButtonSqueeze) != 0;
  sample.jump = physical_jump || parkour.vaulting ||
                (right.buttons & openxr::kButtonPrimary) != 0;
  sample.crouch = physically_crouched || parkour.sliding ||
                  (left.buttons & openxr::kButtonPrimary) != 0;
  const RawFrame frame = scheme_.Update(sample);

  if (!foreground()) {
    if (timestamp_ms >= last_focus_attempt_ms_ + 500) {
      last_focus_attempt_ms_ = timestamp_ms;
      (void)tryFocusTarget();
    }
  }
  if (!foreground()) {
    releaseAll();
    previous_jump_ = sample.jump;
    previous_melee_ = false;
    return;
  }

  const auto movement_it = frame.values.find(ActionId::kMove2D);
  const Vector2 movement =
      movement_it != frame.values.end() &&
              std::holds_alternative<Vector2>(movement_it->second)
          ? std::get<Vector2>(movement_it->second)
          : Vector2{};
  const bool next_keys[6] = {
      movement.y > 0.25f, movement.x < -0.25f, movement.y < -0.25f,
      movement.x > 0.25f,
      frame.values.count(ActionId::kSprint) > 0 &&
          std::get<bool>(frame.values.at(ActionId::kSprint)),
      sample.crouch};
  const std::uint16_t scan_codes[6] = {kW, kA, kS, kD, kShift,
                                       parkour_keys_.slide_scan};
  for (int i = 0; i < 6; ++i) {
    if (next_keys[i] != key_state_[i]) {
      setKey(scan_codes[i], next_keys[i]);
      key_state_[i] = next_keys[i];
    }
  }

  // Space remains a game-owned action: Catalyst decides whether the held
  // jump reaches a vault, wall-run, or ledge autograb. Keeping it held is
  // important for the game's native climb/ledge contract.
  if (sample.jump != jump_key_) {
    setKey(parkour_keys_.vault_scan, sample.jump);
    jump_key_ = sample.jump;
  }
  previous_jump_ = sample.jump;

  const auto& intent = scheme_.latestIntent();
  const bool next_rope = intent.left.mag_rope_held || intent.right.mag_rope_held;
  if (next_rope != mag_rope_) {
    setKey(parkour_keys_.mag_rope_scan, next_rope);
    mag_rope_ = next_rope;
  }

  // A fast tracked-hand strike is an authored melee edge. Catalyst's normal
  // primary attack is the left mouse action, so use the same input route as
  // the trigger while preserving a held-trigger state. This keeps the
  // motion layer game-agnostic and avoids inventing a private game offset.
  const auto melee_it = frame.values.find(ActionId::kMelee);
  const bool melee = melee_it != frame.values.end() &&
                     std::holds_alternative<bool>(melee_it->second) &&
                     std::get<bool>(melee_it->second);
  if (melee && !previous_melee_ && !sample.left_trigger &&
      !sample.right_trigger) {
    setMouseButton(MOUSEEVENTF_LEFTDOWN, true);
    setMouseButton(MOUSEEVENTF_LEFTDOWN, false);
  }
  previous_melee_ = melee;

  const auto turn_it = frame.values.find(ActionId::kTurn2D);
  if (turn_it != frame.values.end() &&
      std::holds_alternative<Vector2>(turn_it->second)) {
    const float turn = std::get<Vector2>(turn_it->second).x;
    if (turn != 0.0f) {
      INPUT mouse{};
      mouse.type = INPUT_MOUSE;
      mouse.mi.dx = static_cast<LONG>(
          comfort_.FilterTurn(turn, timestamp_ms).mouse_delta);
      mouse.mi.dwFlags = MOUSEEVENTF_MOVE;
      (void)SendInput(1, &mouse, sizeof(mouse));
    }
  }

  const bool next_left = sample.left_trigger;
  const bool next_right = sample.right_trigger;
  if (next_left != mouse_left_) {
    setMouseButton(MOUSEEVENTF_LEFTDOWN, next_left);
    mouse_left_ = next_left;
  }
  if (next_right != mouse_right_) {
    setMouseButton(MOUSEEVENTF_RIGHTDOWN, next_right);
    mouse_right_ = next_right;
  }
}

void GameInputSynth::releaseAll() {
  const std::uint16_t scan_codes[6] = {kW, kA, kS, kD, kShift,
                                       parkour_keys_.slide_scan};
  for (int i = 0; i < 6; ++i) {
    if (key_state_[i]) setKey(scan_codes[i], false);
    key_state_[i] = false;
  }
  if (jump_key_) setKey(parkour_keys_.vault_scan, false);
  if (mag_rope_) setKey(parkour_keys_.mag_rope_scan, false);
  if (mouse_left_) setMouseButton(MOUSEEVENTF_LEFTDOWN, false);
  if (mouse_right_) setMouseButton(MOUSEEVENTF_RIGHTDOWN, false);
  mouse_left_ = false;
  mouse_right_ = false;
  previous_melee_ = false;
  jump_key_ = false;
  mag_rope_ = false;
}

}  // namespace mecvr::input
