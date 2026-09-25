#include "config/input_bindings.h"

namespace mecvr::config {

namespace {

using mecvr::input::DominantHand;
using mecvr::input::InputMode;
using mecvr::input::MovementReference;
using mecvr::input::TurnMode;

std::string Trim(const std::string& s) {
  const char* ws = " \t\r\n";
  const auto begin = s.find_first_not_of(ws);
  if (begin == std::string::npos) return "";
  const auto end = s.find_last_not_of(ws);
  return s.substr(begin, end - begin + 1);
}

bool ParseMode(const std::string& s, InputMode* out) {
  if (s == "motion") {
    *out = InputMode::kMotion;
    return true;
  }
  if (s == "hybrid_auto") {
    *out = InputMode::kHybridAuto;
    return true;
  }
  if (s == "xbox_vr") {
    *out = InputMode::kXboxVr;
    return true;
  }
  if (s == "custom") {
    *out = InputMode::kCustom;
    return true;
  }
  return false;
}

const char* FormatMode(InputMode mode) {
  switch (mode) {
    case InputMode::kMotion:
      return "motion";
    case InputMode::kHybridAuto:
      return "hybrid_auto";
    case InputMode::kXboxVr:
      return "xbox_vr";
    case InputMode::kCustom:
      return "custom";
  }
  return "hybrid_auto";
}

bool ParseDominantHand(const std::string& s, DominantHand* out) {
  if (s == "right") {
    *out = DominantHand::kRight;
    return true;
  }
  if (s == "left") {
    *out = DominantHand::kLeft;
    return true;
  }
  return false;
}

bool ParseTurnMode(const std::string& s, TurnMode* out) {
  if (s == "smooth") {
    *out = TurnMode::kSmooth;
    return true;
  }
  if (s == "snap") {
    *out = TurnMode::kSnap;
    return true;
  }
  return false;
}

bool ParseMovementReference(const std::string& s, MovementReference* out) {
  if (s == "head") {
    *out = MovementReference::kHead;
    return true;
  }
  if (s == "body") {
    *out = MovementReference::kBody;
    return true;
  }
  if (s == "left_hand") {
    *out = MovementReference::kLeftHand;
    return true;
  }
  return false;
}

const char* FormatDominantHand(DominantHand hand) {
  return hand == DominantHand::kLeft ? "left" : "right";
}

const char* FormatTurnMode(TurnMode mode) {
  return mode == TurnMode::kSnap ? "snap" : "smooth";
}

const char* FormatMovementReference(MovementReference ref) {
  switch (ref) {
    case MovementReference::kHead:
      return "head";
    case MovementReference::kBody:
      return "body";
    case MovementReference::kLeftHand:
      return "left_hand";
  }
  return "head";
}

// Parses "route:path;route:path". Entries without a ':' separator or with
// an empty route/path are skipped; never fails the whole load.
std::vector<ActionBinding> ParseBindingList(const std::string& text) {
  std::vector<ActionBinding> out;
  std::string::size_type start = 0;
  while (start <= text.size()) {
    auto end = text.find(';', start);
    const std::string entry =
        Trim(text.substr(start, end == std::string::npos
                                    ? std::string::npos
                                    : end - start));
    const auto colon = entry.find(':');
    if (!entry.empty() && colon != std::string::npos) {
      ActionBinding binding{Trim(entry.substr(0, colon)),
                            Trim(entry.substr(colon + 1))};
      if (!binding.route.empty() && !binding.path.empty()) {
        out.push_back(binding);
      }
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

std::string FormatBindingList(const std::vector<ActionBinding>& bindings) {
  std::string out;
  for (const auto& binding : bindings) {
    if (!out.empty()) out += ";";
    out += binding.route + ":" + binding.path;
  }
  return out;
}

void Bind(std::map<std::string, std::vector<ActionBinding>>* actions,
          const char* action,
          std::initializer_list<ActionBinding> bindings) {
  (*actions)[action] = bindings;
}

}  // namespace

bool RegisterInputBindings(Registry& registry) {
  return registry.registerSection(kInputBindingsSection, 1);
}

InputBindings DefaultInputBindings() {
  InputBindings bindings;
  auto* actions = &bindings.actions;
  Bind(actions, "Move2D", {{"touch", "left_stick"}, {"xbox", "left_stick"}});
  Bind(actions, "Turn2D", {{"touch", "right_stick"}, {"xbox", "right_stick"}});
  Bind(actions, "Look2D", {{"xbox", "right_stick"}, {"mouse", "move"}});
  Bind(actions, "TriggerR", {{"touch", "right_trigger"}});
  Bind(actions, "TriggerL", {{"touch", "left_trigger"}});
  Bind(actions, "GripR", {{"touch", "right_squeeze"}});
  Bind(actions, "GripL", {{"touch", "left_squeeze"}});
  Bind(actions, "Jump", {{"touch", "a_button"}, {"xbox", "a_button"}});
  Bind(actions, "Crouch", {{"touch", "b_button"}, {"xbox", "b_button"}});
  Bind(actions, "Slide",
       {{"touch", "left_stick_click"}, {"xbox", "left_stick_click"}});
  Bind(actions, "Interact", {{"touch", "x_button"}, {"xbox", "x_button"}});
  Bind(actions, "Melee", {{"touch", "right_stick_click"}, {"xbox", "y_button"}});
  Bind(actions, "HeavyMelee",
       {{"touch", "y_button"}, {"xbox", "right_bumper"}});
  Bind(actions, "Ability",
       {{"touch", "left_stick_click"}, {"xbox", "left_bumper"}});
  Bind(actions, "Sprint",
       {{"touch", "left_stick_click_hold"}, {"xbox", "left_stick_click"}});
  Bind(actions, "Menu", {{"touch", "menu_button"}, {"xbox", "menu_button"}});
  Bind(actions, "Pause", {{"touch", "menu_button_hold"}, {"xbox", "start"}});
  Bind(actions, "Recenter",
       {{"touch", "system_button"}, {"xbox", "start_hold"}});
  // Derived MEC actions intentionally have no bindings: Vault, WallRun,
  // Climb, Swing, Rope are game-derived, never produced by input.
  return bindings;
}

bool SaveInputBindings(Registry& registry, const InputBindings& bindings) {
  bool ok = true;
  ok = registry.set(kInputBindingsSection, "mode",
                    FormatMode(bindings.mode)) &&
       ok;
  ok = registry.set(kInputBindingsSection, "dominant_hand",
                    FormatDominantHand(bindings.dominant_hand)) &&
       ok;
  ok = registry.set(kInputBindingsSection, "turn_mode",
                    FormatTurnMode(bindings.turn_mode)) &&
       ok;
  ok = registry.set(kInputBindingsSection, "movement_reference",
                    FormatMovementReference(bindings.movement_reference)) &&
       ok;
  for (const auto& [action, list] : bindings.actions) {
    ok = registry.set(kInputBindingsSection, action,
                      FormatBindingList(list)) &&
         ok;
  }
  return ok;
}

bool LoadInputBindings(const Registry& registry, InputBindings* out) {
  if (out == nullptr) return false;
  InputBindings bindings;  // Missing keys keep defaults.
  if (const auto mode = registry.get(kInputBindingsSection, "mode")) {
    if (!ParseMode(Trim(*mode), &bindings.mode)) return false;
  }
  if (const auto hand = registry.get(kInputBindingsSection, "dominant_hand")) {
    if (!ParseDominantHand(Trim(*hand), &bindings.dominant_hand)) return false;
  }
  if (const auto turn = registry.get(kInputBindingsSection, "turn_mode")) {
    if (!ParseTurnMode(Trim(*turn), &bindings.turn_mode)) return false;
  }
  if (const auto ref = registry.get(kInputBindingsSection, "movement_reference")) {
    if (!ParseMovementReference(Trim(*ref), &bindings.movement_reference)) {
      return false;
    }
  }
  // Start from defaults, then overlay stored keys. Malformed entries are
  // skipped per entry, never failing the whole load.
  bindings.actions = DefaultInputBindings().actions;
  for (auto& [action, list] : bindings.actions) {
    if (const auto stored = registry.get(kInputBindingsSection, action)) {
      list = ParseBindingList(*stored);
    }
  }
  *out = bindings;
  return true;
}

}  // namespace mecvr::config
