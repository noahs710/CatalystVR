#pragma once

// IInputDevice: source-facing device interface. Concrete devices translate
// raw hardware into RawFrames keyed by ActionId; per-family arbitration
// (arbitration.h) and MEC gameplay integration (sub-project 3) sit above.
//
// Sub-project 1 scope: skeletons returning canned/test states only. No real
// OpenXR or XInput calls, no MEC gameplay wiring, no game process
// interaction. Only Recenter, diagnostics, and test paths are meaningful.
#include <cstdint>
#include <map>

#include "input/actions.h"

namespace mecvr::input {

// Raw per-device sample. Values carry analog/digital payloads per action;
// poses carry validity/quality separately from button state.
struct RawFrame {
  std::uint64_t timestamp = 0;
  bool connected = false;
  std::map<ActionId, ActionValue> values;
  std::map<ActionId, PoseState> poses;
};

class IInputDevice {
 public:
  virtual ~IInputDevice() = default;
  virtual const char* name() const = 0;
  virtual ActionSource source() const = 0;
  virtual bool connected() const = 0;
  // Canned/neutral state in Sub-project 1; real hardware polling arrives
  // with MEC gameplay integration (sub-project 3).
  virtual RawFrame Poll(std::uint64_t timestamp) = 0;
  // Recenter request route (meaningful now; diagnostics/test only).
  virtual bool RequestRecenter() = 0;
};

}  // namespace mecvr::input
