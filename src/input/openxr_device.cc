#include "input/openxr_device.h"

namespace mecvr::input {

OpenXrInputDevice::OpenXrInputDevice() = default;

const char* OpenXrInputDevice::name() const { return "openxr"; }

ActionSource OpenXrInputDevice::source() const { return ActionSource::kOpenXr; }

bool OpenXrInputDevice::connected() const { return connected_; }

RawFrame OpenXrInputDevice::Poll(std::uint64_t timestamp) {
  if (has_test_frame_) {
    RawFrame frame = test_frame_;
    frame.timestamp = timestamp;
    frame.connected = connected_;
    return frame;
  }
  // Canned neutral state: connected flag only, no actions, poses invalid.
  RawFrame frame;
  frame.timestamp = timestamp;
  frame.connected = connected_;
  return frame;
}

bool OpenXrInputDevice::RequestRecenter() {
  recenter_requested_ = true;
  return true;
}

}  // namespace mecvr::input
