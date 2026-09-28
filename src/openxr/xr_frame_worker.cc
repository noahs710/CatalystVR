// T11 XR frame worker implementation (plan Key Decisions 2-3). See header.

#include "openxr/xr_frame_worker.h"

#include <chrono>
#include <cstring>
#include <utility>

#include "openxr/xr_backend.h"
#include "render/m2b_mono.h"

namespace mecvr::openxr {
namespace {

std::int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

XrFrameWorker::XrFrameWorker(IXrBackend& backend, FrameMailbox& mailbox,
                             StereoMailbox* stereo_mailbox,
                             FrameObserver observer,
                             render::SharedCaptureMailbox<
                                 render::SharedCaptureFrame, 3>* shared_mailbox,
                             std::function<bool(
                                 render::SharedCaptureRegistration*)>
                                 registration_provider,
                             std::function<void(bool)> consumer_ready)
    : backend_(backend),
      mailbox_(mailbox),
      stereo_mailbox_(stereo_mailbox),
      observer_(std::move(observer)),
      shared_mailbox_(shared_mailbox),
      registration_provider_(std::move(registration_provider)),
      consumer_ready_(std::move(consumer_ready)) {}

bool XrFrameWorker::pumpOnce() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_) {
      if (!stopped_) {
        stopped_ = true;
        if (shared_generation_ != 0) {
          backend_.unregisterSharedCapture(shared_generation_);
          shared_generation_ = 0;
        }
        if (consumer_ready_) consumer_ready_(false);
        if (shared_mailbox_ != nullptr) shared_mailbox_->drain();
        stats_.mailbox_drained += mailbox_.drain();
        if (stereo_mailbox_ != nullptr) stereo_mailbox_->drain();
        stats_.mailbox_superseded = mailbox_.superseded();
        stats_.presents_published = mailbox_.published();
      }
      return false;
    }
    if (run_start_ns_ == 0) run_start_ns_ = NowNs();
  }

  // XR-owned pacing: the only waitFrame call site (Key Decision 2).
  const std::int64_t wait_start = NowNs();
  const FrameTiming timing = backend_.waitFrame();
  const std::int64_t wait_end = NowNs();

  std::lock_guard<std::mutex> lock(mutex_);
  stats_.wait_ns = wait_end - wait_start;
  stats_.predicted_interval_ns = timing.predicted_display_period_ns;

  if (!timing.should_render) {
    ++stats_.missed_frames;
    return true;
  }
  if (!backend_.beginFrame()) {
    ++stats_.begin_failed;
    return true;
  }
  // Poses observed for diagnostics only: the mono image is identical for
  // both eyes regardless of head pose (compositor reprojection may still
  // move the submitted quad; the game camera is untouched).
  const LocatedViews views = backend_.locateViews(Space::kLocal);
  if (observer_) observer_(timing, views);

  ++stats_.xr_frames;
  // Explicit stereo owns the eye images. The shared GPU transport is a
  // mono-frame optimization and submitting it first would silently replace
  // the two per-eye captures with the same image in both eyes.
  if (stereo_mailbox_ == nullptr && shared_mailbox_ != nullptr &&
      registration_provider_) {
    render::SharedCaptureRegistration registration;
    if (registration_provider_(&registration) &&
        registration.generation != shared_generation_) {
      if (shared_generation_ != 0)
        backend_.unregisterSharedCapture(shared_generation_);
      if (backend_.registerSharedCapture(registration)) {
        shared_generation_ = registration.generation;
        if (consumer_ready_) consumer_ready_(true);
      } else {
        shared_generation_ = 0;
        ++stats_.gpu_registration_failed;
        if (consumer_ready_) consumer_ready_(false);
      }
    }
    auto gpu_frame = shared_mailbox_->acquireNewest();
    if (gpu_frame && shared_generation_ != 0) {
      const auto& frame = gpu_frame.payload();
      backend_.enableStereoProjection();
      backend_.acquireSwapchainImage(0);
      backend_.acquireSwapchainImage(1);
      const bool submitted = backend_.submitSharedFrame(frame);
      backend_.releaseSwapchainImage(0);
      backend_.releaseSwapchainImage(1);
      if (submitted) {
        backend_.endFrame(true);
        ++stats_.gpu_submitted;
        return true;
      }
      ++stats_.gpu_fallback;
      backend_.unregisterSharedCapture(shared_generation_);
      shared_generation_ = 0;
      if (consumer_ready_) consumer_ready_(false);
    }
  }
  if (stereo_mailbox_ != nullptr) {
    render::StereoFramePtr stereo = stereo_mailbox_->consumeNewest();
    if (stereo) {
      // The producer must stamp both the game simulation epoch and the pose
      // sample with the XR tick token. A mismatch is dropped rather than
      // showing a cross-frame eye pair or silently reusing mono transport.
      // Temporal stereo renders the two eyes on consecutive game presents;
      // the XR worker can wake on the next compositor tick after the pair is
      // complete. Accept only the current tick or a bounded one-tick-old
      // source pair, while retaining the exact pair identity internally.
      const bool current_epoch =
          render::SameStereoEpoch(*stereo, timing.frame_index,
                                  timing.frame_index);
      const bool recent_epoch =
          stereo->epoch < timing.frame_index &&
          timing.frame_index - stereo->epoch <= 1 &&
          stereo->pose_sequence != 0;
      if (!render::StereoFrameValid(*stereo) ||
          (!current_epoch && !recent_epoch)) {
        ++stats_.stereo_rejected;
      } else if (!stereo_projection_enabled_ &&
                 !backend_.enableStereoProjection()) {
        ++stats_.stereo_rejected;
      } else {
        stereo_projection_enabled_ = true;
        const std::uint32_t image0 = backend_.acquireSwapchainImage(0);
        const std::uint32_t image1 = backend_.acquireSwapchainImage(1);
        (void)image0;
        (void)image1;
        const bool up0 = backend_.uploadEyeImage(
            0, stereo->pixels_rgba[0].data(), stereo->width[0],
            stereo->height[0]);
        const bool up1 = backend_.uploadEyeImage(
            1, stereo->pixels_rgba[1].data(), stereo->width[1],
            stereo->height[1]);
        backend_.releaseSwapchainImage(0);
        backend_.releaseSwapchainImage(1);
        if (up0 && up1) {
          backend_.endFrame(true);
          ++stats_.stereo_submitted;
          return true;
        }
        ++stats_.stereo_rejected;
      }
    }
  }
  render::MonoFramePtr frame = mailbox_.consumeNewest();
  if (frame) {
    if (first_capture_ns_ == 0) first_capture_ns_ = frame->capture_time_ns;
    last_capture_ns_ = frame->capture_time_ns;

    // Mono upload: the SAME pixels to both eye scratch buffers (models
    // the XR-worker-side copy into each swapchain image). Timed as the
    // copy stage; byte-compared as the identical-eyes proof.
    const std::size_t bytes = frame->pixels_rgba.size();
    const std::int64_t copy_start = NowNs();
    for (int eye = 0; eye < 2; ++eye) {
      eye_scratch_[eye].resize(bytes);
      if (bytes > 0) {
        std::memcpy(eye_scratch_[eye].data(), frame->pixels_rgba.data(),
                    bytes);
      }
    }
    const std::int64_t copy_end = NowNs();
    stats_.copy_ns = copy_end - copy_start;
    if (stats_.copy_ns > stats_.copy_max_ns)
      stats_.copy_max_ns = stats_.copy_ns;
    const bool identical =
        bytes == 0 || eye_scratch_[0] == eye_scratch_[1];

    // Size the mono composition target BEFORE acquiring (M2B quad; no-op
    // for mocks and the projection path).
    backend_.ensureMonoLayer(frame->width, frame->height);

    const std::int64_t acquire_start = NowNs();
    const std::uint32_t image0 = backend_.acquireSwapchainImage(0);
    const std::uint32_t image1 = backend_.acquireSwapchainImage(1);
    (void)image0;
    (void)image1;
    const std::int64_t acquire_end = NowNs();
    stats_.acquire_ns = acquire_end - acquire_start;

    // Live upload: same scratch pixels into each acquired eye image.
    // Mock/test backends no-op (return true); the real backend copies
    // into the D3D11 swapchain texture. On failure the frame is skipped,
    // never partially submitted.
    const std::int64_t upload_start = NowNs();
    const bool up0 = backend_.uploadEyeImage(
        0, eye_scratch_[0].data(), frame->width, frame->height);
    const bool up1 = backend_.uploadEyeImage(
        1, eye_scratch_[1].data(), frame->width, frame->height);
    const std::int64_t upload_end = NowNs();
    stats_.upload_ns = upload_end - upload_start;
    if (stats_.upload_ns > stats_.upload_max_ns)
      stats_.upload_max_ns = stats_.upload_ns;

    const std::int64_t release_start = NowNs();
    backend_.releaseSwapchainImage(0);
    backend_.releaseSwapchainImage(1);
    const std::int64_t release_end = NowNs();
    stats_.release_ns = release_end - release_start;

    if (!up0 || !up1) {
      ++stats_.upload_failed;
      backend_.endFrame(false);
      return true;
    }
    backend_.endFrame(true);

    EyeSubmitRecord record;
    record.left_sequence = frame->sequence;
    record.right_sequence = frame->sequence;
    record.same_storage = true;  // One MonoFrame referenced for both.
    record.pixels_identical = identical;
    // Diagnostic ring, NOT a history: unbounded growth here would leak over
    // a long session. Totals live in submitted_new/reused counters.
    constexpr std::size_t kEyeLogCap = 256;
    if (stats_.eye_log.size() >= kEyeLogCap) {
      stats_.eye_log.erase(stats_.eye_log.begin(),
                           stats_.eye_log.begin() +
                               static_cast<std::ptrdiff_t>(
                                   stats_.eye_log.size() - kEyeLogCap + 1));
    }
    stats_.eye_log.push_back(record);

    const std::int64_t age =
        timing.predicted_display_time_ns - frame->capture_time_ns;
    stats_.last_frame_age_ns = age;
    if (stats_.submitted_new == 0 || age > stats_.max_frame_age_ns)
      stats_.max_frame_age_ns = age;
    age_sum_ns_ += static_cast<double>(age);
    ++stats_.submitted_new;
    stats_.mean_frame_age_ns =
        age_sum_ns_ / static_cast<double>(stats_.submitted_new);
    last_frame_ = frame;
    stats_.mailbox_superseded = mailbox_.superseded();
  } else if (last_frame_) {
    // No new capture: re-show the newest safe completed frame. A swapchain
    // image must be acquired before use, so re-acquire and re-upload the
    // same frame (never submit a stale un-acquired image).
    backend_.acquireSwapchainImage(0);
    backend_.acquireSwapchainImage(1);
    const bool rup0 = backend_.uploadEyeImage(
        0, eye_scratch_[0].data(), last_frame_->width, last_frame_->height);
    const bool rup1 = backend_.uploadEyeImage(
        1, eye_scratch_[1].data(), last_frame_->width, last_frame_->height);
    backend_.releaseSwapchainImage(0);
    backend_.releaseSwapchainImage(1);
    if (rup0 && rup1) {
      backend_.endFrame(true);
      ++stats_.reused;
    } else {
      ++stats_.upload_failed;
      backend_.endFrame(false);
    }
  } else {
    backend_.endFrame(false);
    ++stats_.empty_ticks;
  }
  stats_.presents_published = mailbox_.published();
  if (stats_.mailbox_high_water < mailbox_.highWater())
    stats_.mailbox_high_water = mailbox_.highWater();
  return true;
}

void XrFrameWorker::run(std::uint64_t max_frames) {
  for (std::uint64_t i = 0; i < max_frames; ++i) {
    if (!pumpOnce()) return;
  }
}

void XrFrameWorker::requestStop() {
  std::lock_guard<std::mutex> lock(mutex_);
  stop_requested_ = true;
}

M2bStats XrFrameWorker::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  M2bStats out = stats_;
  const std::int64_t now = NowNs();
  if (now > run_start_ns_ && stats_.xr_frames > 0) {
    out.xr_rate_hz = static_cast<double>(stats_.xr_frames) * 1e9 /
                     static_cast<double>(now - run_start_ns_);
  }
  if (last_capture_ns_ > first_capture_ns_ && stats_.presents_published > 1) {
    out.present_rate_hz =
        static_cast<double>(stats_.presents_published - 1) * 1e9 /
        static_cast<double>(last_capture_ns_ - first_capture_ns_);
  }
  return out;
}

}  // namespace mecvr::openxr
