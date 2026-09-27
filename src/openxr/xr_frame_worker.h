#pragma once

// T11 XR frame worker (plan Key Decisions 2-3, render stream). Owns ALL
// OpenXR timing on its thread: xrWaitFrame (via IXrBackend::waitFrame),
// beginFrame, locateViews, swapchain acquire/release, endFrame. The game
// Present thread never calls into this worker and never waits on it.
//
// Per XR tick the worker consumes either a validated temporal stereo pair or
// the newest mono mailbox frame. Mono fallback submits the same image to both
// eyes; stereo pairs carry separate eye images and are accepted only when
// their epoch/pose gate passes. With no new frame it re-shows the last
// submitted frame; with nothing ever captured it ends the frame unsubmitted.
// Full instrumentation lands in M2bStats.

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "openxr/mailbox.h"
#include "openxr/stereo_mailbox.h"
#include "render/shared_capture_mailbox.h"

namespace mecvr::openxr {

class IXrBackend;  // Declared in openxr/xr_backend.h (seam, no XR linkage).
struct FrameTiming;
struct LocatedViews;

struct EyeSubmitRecord {
  std::uint64_t left_sequence = 0;
  std::uint64_t right_sequence = 0;
  bool same_storage = false;      // Both eyes referenced one MonoFrame.
  bool pixels_identical = false;  // Both eye uploads byte-identical.
};

struct M2bStats {
  // Game Present side (from mailbox + capture timestamps).
  std::uint64_t presents_published = 0;
  double present_rate_hz = 0.0;
  // XR side.
  std::uint64_t xr_frames = 0;
  std::uint64_t missed_frames = 0;  // should_render=false ticks.
  std::uint64_t begin_failed = 0;
  std::uint64_t empty_ticks = 0;  // Ticks with nothing captured yet.
  double xr_rate_hz = 0.0;
  std::int64_t predicted_interval_ns = 0;  // Last xrWaitFrame period.
  // Submit side.
  std::uint64_t submitted_new = 0;
  std::uint64_t reused = 0;  // Ticks re-showing the last frame.
  std::int64_t last_frame_age_ns = 0;  // predicted_time - capture_time.
  std::int64_t max_frame_age_ns = 0;
  double mean_frame_age_ns = 0.0;
  // Mailbox side.
  std::uint64_t mailbox_superseded = 0;
  std::uint64_t mailbox_drained = 0;
  std::size_t mailbox_high_water = 0;
  // Timing side (nanoseconds, steady clock around each stage).
  std::int64_t wait_ns = 0;     // waitFrame (XR-owned pacing).
  std::int64_t upload_ns = 0;   // Eye upload into swapchain (last submit).
  std::int64_t upload_max_ns = 0;
  std::uint64_t upload_failed = 0;  // Upload failures (frame skipped).
  std::int64_t copy_ns = 0;     // Mono upload memcpy (last submit).
  std::int64_t copy_max_ns = 0;
  std::int64_t acquire_ns = 0;  // Both-eye acquire (last submit).
  std::int64_t release_ns = 0;  // Both-eye release (last submit).
  std::vector<EyeSubmitRecord> eye_log;
  std::uint64_t stereo_submitted = 0;
  std::uint64_t stereo_rejected = 0;
  std::uint64_t gpu_submitted = 0;
  std::uint64_t gpu_fallback = 0;
  std::uint64_t gpu_registration_failed = 0;
};

class XrFrameWorker {
 public:
  using FrameObserver =
      std::function<void(const FrameTiming&, const LocatedViews&)>;

  XrFrameWorker(IXrBackend& backend, FrameMailbox& mailbox,
                StereoMailbox* stereo_mailbox = nullptr,
                FrameObserver observer = {},
                render::SharedCaptureMailbox<render::SharedCaptureFrame, 3>*
                    shared_mailbox = nullptr,
                std::function<bool(render::SharedCaptureRegistration*)>
                    registration_provider = {},
                std::function<void(bool)> consumer_ready = {});

  XrFrameWorker(const XrFrameWorker&) = delete;
  XrFrameWorker& operator=(const XrFrameWorker&) = delete;

  // One XR tick on the calling (XR worker) thread. Returns false once
  // requestStop() has been observed (drains the mailbox first).
  bool pumpOnce();

  // Bounded run: at most max_frames ticks or until requestStop().
  void run(std::uint64_t max_frames);

  void requestStop();
  M2bStats stats() const;

 private:
  IXrBackend& backend_;
  FrameMailbox& mailbox_;
  StereoMailbox* stereo_mailbox_ = nullptr;
  FrameObserver observer_;
  mutable std::mutex mutex_;
  bool stop_requested_ = false;
  bool stopped_ = false;
  M2bStats stats_;
  render::MonoFramePtr last_frame_;
  std::vector<std::uint8_t> eye_scratch_[2];
  std::int64_t run_start_ns_ = 0;
  std::int64_t first_capture_ns_ = 0;
  std::int64_t last_capture_ns_ = 0;
  double age_sum_ns_ = 0.0;
  bool stereo_projection_enabled_ = false;
  render::SharedCaptureMailbox<render::SharedCaptureFrame, 3>*
      shared_mailbox_ = nullptr;
  std::function<bool(render::SharedCaptureRegistration*)>
      registration_provider_;
  std::function<void(bool)> consumer_ready_;
  std::uint64_t shared_generation_ = 0;
};

}  // namespace mecvr::openxr
