#pragma once

// XInput (Xbox gamepad) skeleton device. Returns canned neutral states;
// only diagnostics and injected test frames are meaningful. No real
// XInput calls in Sub-project 1 (no <xinput.h> dependency).
#include "input/device.h"

namespace mecvr::input {

class XInputDevice : public IInputDevice {
 public:
  XInputDevice();

  const char* name() const override;
  ActionSource source() const override;
  bool connected() const override;
  RawFrame Poll(std::uint64_t timestamp) override;
  bool RequestRecenter() override;

  // Test/diagnostics injection. The next Poll returns test_frame when set.
  void set_connected(bool connected) { connected_ = connected; }
  void SetTestFrame(const RawFrame& frame) {
    test_frame_ = frame;
    has_test_frame_ = true;
  }
  void ClearTestFrame() { has_test_frame_ = false; }

 private:
  bool connected_ = false;  // No gamepad claim in Sub-project 1.
  bool has_test_frame_ = false;
  RawFrame test_frame_;
};

}  // namespace mecvr::input
