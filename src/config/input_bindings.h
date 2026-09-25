#pragma once

// Isolated input-config module on top of the frozen Registry
// (src/config/config.h, which this file includes but never modifies).
// Owns ONLY the "input_bindings" section: action -> bindings map plus the
// input preference enums. No gameplay wiring.
#include <map>
#include <string>
#include <vector>

#include "config/config.h"
#include "input/actions.h"

namespace mecvr::config {

// Registry section owned by this module.
inline constexpr char kInputBindingsSection[] = "input_bindings";

// One route binding: route in {"touch","xbox","mouse","game"} plus a
// route-local path, e.g. {"touch","left_stick"} or {"xbox","a_button"}.
// Serialized as "route:path"; multiple bindings join with ';'.
struct ActionBinding {
  std::string route;
  std::string path;
};

inline bool operator==(const ActionBinding& a, const ActionBinding& b) {
  return a.route == b.route && a.path == b.path;
}

struct InputBindings {
  mecvr::input::InputMode mode = mecvr::input::InputMode::kHybridAuto;
  mecvr::input::DominantHand dominant_hand = mecvr::input::DominantHand::kRight;
  mecvr::input::TurnMode turn_mode = mecvr::input::TurnMode::kSmooth;
  mecvr::input::MovementReference movement_reference =
      mecvr::input::MovementReference::kHead;
  // Keyed by ActionName() ASCII key. Unknown keys stay stored in the
  // Registry/file untouched (the frozen Registry has no key enumeration,
  // so Load overlays known actions onto defaults); Save never deletes them.
  std::map<std::string, std::vector<ActionBinding>> actions;
};

bool RegisterInputBindings(Registry& registry);
InputBindings DefaultInputBindings();
bool SaveInputBindings(Registry& registry, const InputBindings& bindings);
bool LoadInputBindings(const Registry& registry, InputBindings* out);

}  // namespace mecvr::config
