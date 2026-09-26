// M2B headless validation (plan T11, render stream). End-to-end mono
// transport on MockXRBackend with synthetic frames — no headset, no D3D
// device, no game process.
//
// Thread map (mirrors the live topology):
//  - publisher thread = game Present thread: ONLY mailbox.tryPublish +
//    MakeSyntheticFrame. Never touches IXrBackend (would be a STOP
//    violation: Present must never call xrWaitFrame).
//  - worker thread = XR frame worker: ONLY XrFrameWorker::pumpOnce
//    (owns all waitFrame/begin/acquire/release/endFrame calls), paced to
//    the mock display cadence like a real runtime paces xrWaitFrame.
//
// Asserts: identical eyes on every submit, newest-wins delivery,
// non-blocking publish, sane instrumentation, reuse path, bounded
// shutdown drain, and the STOP S3 camera attestation.

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#include <windows.h>

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

void SleepMs(double ms) {
  std::this_thread::sleep_for(
      std::chrono::duration<double, std::milli>(ms));
}

}  // namespace

int main() {
  using namespace mecvr;
  // 1 ms timer resolution: Windows Sleep() defaults to ~15.6 ms granularity,
  // which would cap the synthetic 200 Hz publisher at ~64 Hz and skew the
  // mock virtual clock against the wall-clock capture timestamps. This is
  // test-harness accuracy only; production pacing is owned by xrWaitFrame.
  const MMRESULT timer_res = timeBeginPeriod(1);
  constexpr std::uint32_t kW = 160;
  constexpr std::uint32_t kH = 90;
  constexpr std::uint32_t kSeed = 0xC0FFEEu;
  constexpr int kPublishFrames = 400;      // ~200 fps game cadence.
  constexpr double kPublishPeriodMs = 5.0;
  constexpr double kXrPeriodMs = 1000.0 / 90.0;  // Mock 90 Hz runtime.

  // ---- Phase 0: mailbox unit semantics --------------------------------
  {
    openxr::FrameMailbox box(2);
    for (std::uint64_t s = 1; s <= 5; ++s) {
      Check(box.tryPublish(render::MakeSyntheticFrame(s, 8, 8, kSeed)),
            "mailbox: publish seq " + std::to_string(s));
    }
    render::MonoFramePtr newest = box.consumeNewest();
    Check(newest && newest->sequence == 5, "mailbox: newest-wins",
          "got seq=" + std::to_string(newest ? newest->sequence : 0));
    Check(box.superseded() == 4, "mailbox: superseded counted",
          "superseded=" + std::to_string(box.superseded()));
    Check(box.consumeNewest() == nullptr, "mailbox: empty consume null");

    // No-block: 20k back-to-back publishes must finish quickly (no XR
    // wait exists on this path by construction; this bounds the mutex).
    const std::int64_t t0 = render::SteadyNanos();
    bool all_ok = true;
    for (int i = 0; i < 20000; ++i) {
      all_ok = box.tryPublish(
                   render::MakeSyntheticFrame(
                       static_cast<std::uint64_t>(100 + i), 8, 8, kSeed)) &&
               all_ok;
    }
    const std::int64_t dt_ms =
        (render::SteadyNanos() - t0) / 1000000;
    Check(all_ok && dt_ms < 2000, "mailbox: publish never blocks",
          std::to_string(dt_ms) + "ms for 20k publishes");
    Check(box.highWater() <= 2, "mailbox: bounded depth",
          "high_water=" + std::to_string(box.highWater()));
    Check(!box.tryPublish(nullptr), "mailbox: null frame rejected");
  }

  // ---- Phase 1: end-to-end mono transport on the mock ------------------
  openxr::MockXRBackend backend;
  openxr::MockConfig config;
  config.display_frequency_hz = 90.0f;
  // Align the mock time base with the capture clock so frame age at
  // submit is a real latency (predicted_time - capture_time).
  config.start_time_ns = render::SteadyNanos();
  backend.configure(config);
  Check(backend.startup(), "e2e: mock backend startup");

  openxr::FrameMailbox mailbox(2);
  openxr::XrFrameWorker worker(backend, mailbox);

  // Publisher = game thread. ONLY mailbox + synthetic capture.
  std::thread publisher([&] {
    for (int i = 1; i <= kPublishFrames; ++i) {
      mailbox.tryPublish(render::MakeSyntheticFrame(
          static_cast<std::uint64_t>(i), kW, kH, kSeed));
      SleepMs(kPublishPeriodMs);
    }
  });

  // Worker = XR thread, paced to the runtime cadence. Runs past the end
  // of publishing so the final submit converges to the newest frame.
  constexpr int kWorkerTicks = 260;  // ~2.9 s at 90 Hz > 2.0 s publish run.
  std::thread xr_thread([&] {
    for (int i = 0; i < kWorkerTicks; ++i) {
      const std::int64_t tick_start = render::SteadyNanos();
      worker.pumpOnce();
      const double elapsed_ms =
          static_cast<double>(render::SteadyNanos() - tick_start) / 1e6;
      if (elapsed_ms < kXrPeriodMs) SleepMs(kXrPeriodMs - elapsed_ms);
    }
  });
  publisher.join();
  xr_thread.join();

  // Converge: pump until the last published frame is submitted (bounded).
  // The worker normally outlasts the publisher; this covers scheduling
  // jitter without weakening the newest-wins assertion.
  for (int i = 0; i < 400; ++i) {
    openxr::M2bStats s = worker.stats();
    if (!s.eye_log.empty() &&
        s.eye_log.back().left_sequence ==
            static_cast<std::uint64_t>(kPublishFrames)) {
      break;
    }
    worker.pumpOnce();
  }

  openxr::M2bStats stats = worker.stats();
  std::cout << "e2e: published=" << mailbox.published()
            << " consumed_new=" << mailbox.consumedNew()
            << " superseded=" << mailbox.superseded()
            << " submitted_new=" << stats.submitted_new
            << " reused=" << stats.reused
            << " empty_ticks=" << stats.empty_ticks
            << " missed=" << stats.missed_frames
            << " high_water=" << stats.mailbox_high_water << "\n";
  std::cout << "e2e: present_rate_hz=" << stats.present_rate_hz
            << " xr_rate_hz=" << stats.xr_rate_hz
            << " predicted_interval_ns=" << stats.predicted_interval_ns
            << "\n";
  std::cout << "e2e: last_age_ns=" << stats.last_frame_age_ns
            << " max_age_ns=" << stats.max_frame_age_ns
            << " mean_age_ns=" << stats.mean_frame_age_ns
            << " copy_ns=" << stats.copy_ns
            << " copy_max_ns=" << stats.copy_max_ns
            << " acquire_ns=" << stats.acquire_ns
            << " wait_ns=" << stats.wait_ns
            << " release_ns=" << stats.release_ns << "\n";

  // Mailbox accounting must balance exactly: every published frame was
  // submitted once (newest consumed), superseded, or still queued.
  // (No drain yet, so drained=0.)
  Check(mailbox.published() == static_cast<std::uint64_t>(kPublishFrames),
        "e2e: all presents published");
  Check(mailbox.consumedNew() == stats.submitted_new,
        "e2e: every consumed frame submitted exactly once");
  Check(mailbox.published() ==
            mailbox.consumedNew() + mailbox.superseded() + mailbox.depth(),
        "e2e: mailbox accounting balances",
        "published=" + std::to_string(mailbox.published()) +
            " consumed=" + std::to_string(mailbox.consumedNew()) +
            " superseded=" + std::to_string(mailbox.superseded()) +
            " depth=" + std::to_string(mailbox.depth()));

  // IDENTICAL mono image to both eyes on EVERY submit.
  bool eyes_ok = !stats.eye_log.empty();
  for (const openxr::EyeSubmitRecord& r : stats.eye_log) {
    eyes_ok = eyes_ok && (r.left_sequence == r.right_sequence) &&
              r.same_storage && r.pixels_identical;
  }
  Check(eyes_ok, "e2e: identical image to both eyes on every submit",
        std::to_string(stats.eye_log.size()) + " submits checked");

  // Newest-wins end to end: the worker ran past the publisher, so the
  // last submit must be the last published frame.
  Check(!stats.eye_log.empty() &&
            stats.eye_log.back().left_sequence ==
                static_cast<std::uint64_t>(kPublishFrames),
        "e2e: newest frame wins end-to-end",
        "last_submitted=" +
            std::to_string(stats.eye_log.empty()
                               ? 0
                               : stats.eye_log.back().left_sequence));

  // Instrumentation sanity.
  Check(stats.present_rate_hz > 100.0 && stats.present_rate_hz < 400.0,
        "e2e: game Present rate sane",
        std::to_string(stats.present_rate_hz) + " Hz (target ~200)");
  Check(stats.xr_rate_hz > 45.0 && stats.xr_rate_hz < 135.0,
        "e2e: XR rate sane",
        std::to_string(stats.xr_rate_hz) + " Hz (target ~90)");
  Check(stats.predicted_interval_ns > 10000000 &&
            stats.predicted_interval_ns < 12000000,
        "e2e: predicted interval sane",
        std::to_string(stats.predicted_interval_ns) + " ns (~11.1ms)");
  // Age = runtime-predicted time minus wall-clock capture time. Against the
  // MockXRBackend's VIRTUAL clock (deterministic by T5 contract: monotonic,
  // constant period), the sign is not meaningful when the test pumps
  // off-cadence — only boundedness is provable headless. Non-negative ages
  // become a LIVE-gate assertion with the wall-based real runtime. Manual
  // abs keeps this TU dependency-free.
  const std::int64_t abs_last_age = stats.last_frame_age_ns < 0
                                        ? -stats.last_frame_age_ns
                                        : stats.last_frame_age_ns;
  Check(abs_last_age < 1000000000 && stats.max_frame_age_ns < 1000000000,
        "e2e: captured-frame age at submit bounded",
        "last=" + std::to_string(stats.last_frame_age_ns) +
            "ns max=" + std::to_string(stats.max_frame_age_ns) + "ns");
  Check(stats.copy_ns > 0, "e2e: copy time measured",
        std::to_string(stats.copy_ns) + "ns for 160x90 RGBA");
  Check(stats.missed_frames == 0, "e2e: no missed XR frames on mock",
        "(mock never gates should_render; counter exercised)");
  Check(stats.begin_failed == 0, "e2e: no beginFrame failures");
  Check(stats.mailbox_high_water >= 1 && stats.mailbox_high_water <= 2,
        "e2e: mailbox depth bounded",
        "high_water=" + std::to_string(stats.mailbox_high_water));

  // ---- Phase 2: reuse path (XR ticks with no new capture) -------------
  const std::uint64_t reused_before = worker.stats().reused;
  for (int i = 0; i < 10; ++i) worker.pumpOnce();
  Check(worker.stats().reused > reused_before,
        "e2e: idle XR ticks re-show last frame",
        "reused=" + std::to_string(worker.stats().reused));

  // ---- Phase 3: bounded shutdown ---------------------------------------
  worker.requestStop();
  Check(!worker.pumpOnce(), "e2e: worker stops on request");
  stats = worker.stats();
  Check(mailbox.depth() == 0, "e2e: mailbox drained on shutdown",
        "drained=" + std::to_string(stats.mailbox_drained));
  backend.shutdown();
  Check(!backend.running(), "e2e: backend shutdown stops");

  // ---- Phase 4: STOP S3 attestation ------------------------------------
  Check(render::M2bCameraUntouched(),
        "s3: M2B modules expose no game-camera path");

  if (timer_res == TIMERR_NOERROR) timeEndPeriod(1);
  if (g_failures == 0) {
    std::cout << "ALL TESTS PASSED\n";
    return 0;
  }
  std::cout << g_failures << " TEST(S) FAILED\n";
  return 1;
}
