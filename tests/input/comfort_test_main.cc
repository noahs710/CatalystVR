#include <iostream>

#include "input/comfort.h"

namespace {
bool Check(bool value, const char* label) {
  if (!value) std::cerr << "FAIL: " << label << "\n";
  return value;
}
}

int main() {
  using mecvr::input::ComfortConfig;
  using mecvr::input::ComfortFilter;
  using mecvr::input::TurnMode;
  bool ok = true;

  ComfortFilter smooth;
  ok &= Check(smooth.FilterTurn(0.5f, 10).mouse_delta == 7.0f,
              "smooth turn preserves proportional input");

  ComfortConfig snap_config;
  snap_config.turn_mode = TurnMode::kSnap;
  snap_config.snap_degrees = 45.0f;
  snap_config.snap_pixels_per_degree = 1.0f;
  snap_config.snap_cooldown_ms = 250;
  ComfortFilter snap(snap_config);
  ok &= Check(snap.FilterTurn(0.8f, 100).snapped, "first snap fires");
  ok &= Check(!snap.FilterTurn(0.8f, 200).snapped,
              "held stick does not repeat during cooldown");
  ok &= Check(!snap.FilterTurn(0.0f, 210).snapped,
              "neutral re-arms without firing");
  ok &= Check(snap.FilterTurn(-0.8f, 400).snapped,
              "opposite snap fires after re-arm");
  std::cout << (ok ? "COMFORT_TEST: PASS\n" : "COMFORT_TEST: FAIL\n");
  return ok ? 0 : 1;
}
