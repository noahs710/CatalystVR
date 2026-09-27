#pragma once

namespace mecvr::input {

// Edge-triggered recenter chord. Holding both menu buttons requests exactly
// one recenter; releasing either button arms the next request.
class RecenterLatch {
 public:
  bool update(bool left_menu, bool right_menu) {
    const bool held = left_menu && right_menu;
    const bool fired = held && !previous_held_;
    previous_held_ = held;
    return fired;
  }

  void reset() { previous_held_ = false; }

 private:
  bool previous_held_ = false;
};

}  // namespace mecvr::input
