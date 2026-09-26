#pragma once

// Real OpenXR backend (plan T8, M1B deliverable). Standalone only: no game
// device, no game thread, no camera/stereo/gameplay work (STOP S3).
//
// Lifecycle (Key Decisions 1-4/9-10): xrCreateInstance (with
// XR_KHR_D3D11_enable) -> xrGetSystem (HMD) ->
// xrGetD3D11GraphicsRequirementsKHR (adapter LUID + min feature level gate)
// -> own D3D11 device on the runtime-required adapter -> xrCreateSession ->
// LOCAL (+ STAGE where available, VIEW fallback) spaces -> per-eye
// swapchains -> xrWaitFrame frame loop. All xr* calls are expected on a
// dedicated XR worker thread owned by the caller (Key Decision 3); the
// Present hook must never call waitFrame.
//
// Headset-absent (or any init failure) degrades gracefully: startup()
// returns false, running() is false, and diagnostics() explains exactly
// which step failed and why. No exceptions, no crashes, no partial session.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "openxr/xr_backend.h"

namespace mecvr::openxr {

// Snapshot of real-backend bring-up and session state for diagnostics and
// the M1B test. Every step the backend reaches sets its flag (flags are a
// bring-up record, not live-handle state: handles are still released on
// failure), so a failure report shows exactly what was and was not
// exercised (no faking).
struct RealBackendDiagnostics {
  bool instance_created = false;
  bool system_acquired = false;
  bool requirements_queried = false;
  bool device_created = false;
  bool session_created = false;
  bool session_begun = false;  // READY observed, xrBeginSession succeeded.
  bool stage_available = false;
  bool focus_received = false;  // FOCUSED state observed at least once.
  bool session_loss = false;
  bool instance_loss = false;
  std::uint64_t frames_pumped = 0;
  std::string runtime_name;
  std::uint32_t runtime_version_major = 0;
  std::uint32_t runtime_version_minor = 0;
  std::uint32_t runtime_version_patch = 0;
  std::string session_state = "UNKNOWN";
  std::string adapter_description;
  std::uint32_t adapter_vendor_id = 0;
  std::uint32_t adapter_device_id = 0;
  std::string failure_reason;  // Empty when startup() succeeded.
};

class RealOpenXRBackend final : public IXrBackend {
 public:
  RealOpenXRBackend();
  ~RealOpenXRBackend() override;

  // Extra (non-seam) observability. Safe to call from any thread.
  RealBackendDiagnostics diagnostics() const;
  bool stageAvailable() const;
  bool hasFocus() const;

  // IXrBackend. startup() runs full bring-up on the calling thread;
  // waitFrame/beginFrame/locateViews/acquire/release/endFrame belong on the
  // caller's dedicated XR worker thread.
  bool startup() override;
  void shutdown() override;
  bool running() const override;
  FrameTiming waitFrame() override;
  bool beginFrame() override;
  LocatedViews locateViews(Space space) override;
  std::uint32_t acquireSwapchainImage(std::uint32_t view_index) override;
  void releaseSwapchainImage(std::uint32_t view_index) override;
  bool endFrame(bool submitted) override;
  void recenter() override;
  ControllerState controllerState(Hand hand) override;
  float displayFrequencyHz() const override;
  ViewConfig viewConfig(std::uint32_t view_index) const override;
  std::uint32_t viewCount() const override;

 private:
  // Opaque native handles (XrInstance etc.). void* keeps OpenXR headers out
  // of the public header so dependents link only the seam.
  struct Native;
  Native* native_ = nullptr;

  // Event pump; caller holds mutex_.
  static bool PollEventsLocked(Native& native, RealBackendDiagnostics& diag);

  mutable std::mutex mutex_;
  std::atomic<bool> running_{false};
  RealBackendDiagnostics diagnostics_;
  ViewConfig view_configs_[2];
  std::uint32_t view_count_ = 0;
  double ema_period_ns_ = 0.0;  // xrWaitFrame-derived cadence (Key Dec. 4).
  FrameTiming last_timing_{};
  bool frame_open_ = false;
  bool recenter_requested_ = false;
};

}  // namespace mecvr::openxr
