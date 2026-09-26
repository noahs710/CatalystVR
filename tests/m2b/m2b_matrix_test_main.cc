// M2B rate-matrix + stall + shutdown-pending + soak (plan T11 extras).
// Deterministic single-threaded interleaving on MockXRBackend: no sleeps,
// no wall-clock rates (those belong to m2b_mono_test_main.cc). Proves the
// bounded-mailbox invariant under every cadence relationship: accounting
// balances exactly, depth never exceeds capacity, eyes identical, newest
// converges. No game, no headset, no camera/stereo/gameplay code.
#include <cstdint>
#include <iostream>
#include <string>

#include "openxr/mailbox.h"
#include "openxr/mock_xr_backend.h"
#include "openxr/xr_backend.h"
#include "openxr/xr_frame_worker.h"
#include "render/m2b_mono.h"

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& name, const std::string& detail = "") {
  if (ok) {
    std::cout << "[PASS] " << name;
  } else {
    ++g_failures;
    std::cout << "[FAIL] " << name;
  }
  if (!detail.empty()) std::cout << " (" << detail << ")";
  std::cout << "\n";
}

struct Rig {
  mecvr::openxr::MockXRBackend backend;
  mecvr::openxr::FrameMailbox mailbox;
  mecvr::openxr::XrFrameWorker worker;
  std::uint64_t next_seq = 1;

  Rig() : mailbox(2), worker(backend, mailbox) {
    mecvr::openxr::MockConfig config;
    config.display_frequency_hz = 90.0f;
    config.start_time_ns = mecvr::render::SteadyNanos();
    backend.configure(config);
    backend.startup();
  }

  void Publish() {
    mailbox.tryPublish(
        mecvr::render::MakeSyntheticFrame(next_seq++, 32, 32, 0xBEEFu));
  }

  // Pump until the last published frame is the last submitted (bounded).
  bool Converge(int cap = 2000) {
    for (int i = 0; i < cap; ++i) {
      mecvr::openxr::M2bStats s = worker.stats();
      if (!s.eye_log.empty() &&
          s.eye_log.back().left_sequence == next_seq - 1) {
        return true;
      }
      if (!worker.pumpOnce()) return false;
    }
    return false;
  }

  bool Balanced() const {
    return mailbox.published() == mailbox.consumedNew() +
                                      mailbox.superseded() + mailbox.depth();
  }

  bool EyesIdentical() const {
    mecvr::openxr::M2bStats s = worker.stats();
    if (s.eye_log.empty()) return false;
    for (const auto& r : s.eye_log) {
      if (r.left_sequence != r.right_sequence || !r.same_storage ||
          !r.pixels_identical) {
        return false;
      }
    }
    return true;
  }
};

// game_hz/xr_hz cadence relationship over xr_ticks worker ticks.
void RateCase(double game_hz, double xr_hz, int xr_ticks,
              const std::string& name) {
  Rig rig;
  double acc = 0.0;
  for (int t = 0; t < xr_ticks; ++t) {
    acc += game_hz;
    while (acc >= xr_hz) {
      rig.Publish();
      acc -= xr_hz;
    }
    rig.worker.pumpOnce();
  }
  const bool converged = rig.Converge();
  mecvr::openxr::M2bStats s = rig.worker.stats();
  Check(rig.Balanced(), name + ": accounting balances",
        "published=" + std::to_string(rig.mailbox.published()));
  Check(s.mailbox_high_water <= 2, name + ": mailbox bounded",
        "high_water=" + std::to_string(s.mailbox_high_water));
  Check(rig.EyesIdentical(), name + ": eyes identical on every submit");
  Check(converged, name + ": newest converges");
  if (game_hz > xr_hz) {
    Check(s.mailbox_superseded > 0, name + ": oversupply superseded");
  }
  if (game_hz < xr_hz) {
    Check(s.reused > 0, name + ": undersupply reused");
  }
}

}  // namespace

int main() {
  RateCase(200.0, 90.0, 1200, "matrix 200/90");
  RateCase(60.0, 90.0, 1200, "matrix 60/90");
  RateCase(90.0, 90.0, 1200, "matrix 90/90");
  RateCase(45.0, 90.0, 1600, "matrix 45/90");

  // Temporary game-frame stall: publish, stall 300 ticks, resume.
  {
    Rig rig;
    for (int i = 0; i < 200; ++i) {
      rig.Publish();
      rig.worker.pumpOnce();
    }
    const std::uint64_t reused_before = rig.worker.stats().reused;
    for (int i = 0; i < 300; ++i) rig.worker.pumpOnce();
    Check(rig.worker.stats().reused > reused_before,
          "stall: idle ticks re-show last frame");
    for (int i = 0; i < 200; ++i) {
      rig.Publish();
      rig.worker.pumpOnce();
    }
    Check(rig.Converge(), "stall: converges after resume");
    Check(rig.Balanced(), "stall: accounting balances after resume");
    Check(rig.worker.stats().mailbox_high_water <= 2,
          "stall: mailbox bounded across stall");
  }

  // Shutdown with captures pending.
  {
    Rig rig;
    for (int i = 0; i < 60; ++i) rig.Publish();
    rig.worker.requestStop();
    bool stopped = false;
    for (int i = 0; i < 100; ++i) {
      if (!rig.worker.pumpOnce()) {
        stopped = true;
        break;
      }
    }
    Check(stopped, "shutdown: worker stops with captures pending");
    Check(rig.mailbox.depth() == 0, "shutdown: mailbox drained",
          "drained=" +
              std::to_string(rig.worker.stats().mailbox_drained));
  }

  // Long soak: 60k publishes, interleaved pumps, exact accounting.
  {
    Rig rig;
    double acc = 0.0;
    constexpr int kTicks = 30000;  // 60k publishes at 200/90.
    for (int t = 0; t < kTicks; ++t) {
      acc += 200.0;
      while (acc >= 90.0) {
        rig.Publish();
        acc -= 90.0;
      }
      rig.worker.pumpOnce();
    }
    Check(rig.Converge(5000), "soak: converges after 60k publishes");
    Check(rig.Balanced(), "soak: accounting balances exactly",
          "published=" + std::to_string(rig.mailbox.published()));
    Check(rig.worker.stats().mailbox_high_water <= 2,
          "soak: mailbox bounded over long run");
    Check(rig.EyesIdentical(), "soak: eyes identical across ring window");
    Check(rig.worker.stats().eye_log.size() <= 256,
          "soak: diagnostic eye log capped",
          "size=" +
              std::to_string(rig.worker.stats().eye_log.size()));
  }

  if (g_failures == 0) {
    std::cout << "ALL TESTS PASSED\n";
    return 0;
  }
  std::cout << g_failures << " TEST(S) FAILED\n";
  return 1;
}
