#include "input/xinput_device.h"

namespace mecvr::input {

XInputDevice::XInputDevice() = default;

const char* XInputDevice::name() const { return "xinput"; }

ActionSource XInputDevice::source() const { return ActionSource::kXInput; }

bool XInputDevice::connected() const { return connected_; }

RawFrame XInputDevice::Poll(std::uint64_t timestamp) {
  if (has_test_frame_) {
    RawFrame frame = test_frame_;
    frame.timestamp = timestamp;
    frame.connected = connected_;
    return frame;
  }
  // Canned neutral state: connected flag only. XInput never owns a pose,
  // so poses are always absent here.
  RawFrame frame;
  frame.timestamp = timestamp;
  frame.connected = connected_;
  return frame;
}

bool XInputDevice::RequestRecenter() {
  // Xbox route accepts the system recenter request (either-route rule).
  return true;
}

}  // namespace mecvr::input
