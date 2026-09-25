#pragma once

// OpenXR (HMD + Touch) skeleton device. Returns canned neutral states;
// only Recenter, diagnostics, and injected test frames are meaningful.
// No real OpenXR calls in Sub-project 1 (no <openxr.h> dependency).
#include "input/device.h"

namespace mecvr::input {

class OpenXrInputDevice : public IInputDevice {
 public:
  OpenXrInputDevice();

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
  bool recenter_requested() const { return recenter_requested_; }

 private:
  bool connected_ = false;  // No runtime session in Sub-project 1.
  bool has_test_frame_ = false;
  RawFrame test_frame_;
  bool recenter_requested_ = false;
};

}  // namespace mecvr::input
