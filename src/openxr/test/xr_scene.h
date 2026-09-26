#pragma once

// M1C OpenXR test scene (plan T9). Standalone only: no game, no game thread,
// no camera or gameplay work (STOP S3 respected — this scene renders test
// content into its own offscreen D3D11 targets, never into a game swapchain).
//
// Two pieces:
//   SceneRenderer — own D3D11 device plus one render-target/staging pair per
//     eye. Each eye gets an eye-distinguishing tint (reddish left, bluish
//     right): the render target is cleared to the eye color and a centered
//     quad is drawn in a contrasting per-eye color whose brightness follows
//     the head yaw flowing in from the backend. Readback verifies exact
//     pixels. The device/immediate context is touched on exactly one thread
//     (the XR worker during a run), so no multithread protection is needed.
//   SceneSession — deterministic driver that runs the full IXrBackend frame
//     loop (waitFrame/beginFrame/locateViews/acquire/release/endFrame) on a
//     dedicated XR worker thread through a scripted phase table covering
//     session-state transitions, recenter, focus loss, headset absence, and
//     recovery. The mock backend has no runtime of its own, so absence and
//     focus are simulated deterministically by the harness (backend calls
//     are suspended while "absent"; endFrame(submitted=false) while
//     unfocused), exactly as the real backend gates should_render=false.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "openxr/xr_backend.h"

namespace mecvr::openxr::scene {

// Harness-level session states. Mirror the XrSessionState vocabulary so the
// mock script and the real-backend diagnostics report speak the same
// language; on the mock path transitions are driven deterministically by the
// phase script, on the real path they are observed from the runtime.
enum class SessionState {
  kUnknown,
  kIdle,
  kReady,
  kSynchronized,
  kVisible,
  kFocused,
  kStopping,
  kLossPending,
  kExiting
};

const char* SessionStateName(SessionState state);

// Yaw (radians, Y-up right-handed like OpenXR) extracted from a quaternion.
// Inverts YawQuaternion: YawFromQuaternion(YawQuaternion(a)) == a.
float YawFromQuaternion(const XrQuaternionf& q);

// One scripted run phase. submit=false pumps frames without submitting a
// layer (endFrame(false): the focus-loss/idle path). backend_reachable=false
// suspends ALL backend calls for the phase (headset-absent simulation);
// time must stay monotonic across the gap on recovery.
struct PhaseSpec {
  SessionState state = SessionState::kIdle;
  std::uint64_t frames = 0;
  bool submit = false;
  bool backend_reachable = true;
  bool recenter_at_start = false;
  const char* label = "";
};

// Deterministic mock script: idle -> ready -> synchronized -> visible ->
// focused -> focus-lost -> focus-restored(+recenter) -> headset-absent ->
// recovered -> stopping. Covers every T9 checklist transition.
std::vector<PhaseSpec> DefaultMockScript();

// Minimal pump script for the real backend (no fault injection there;
// absence/focus on real hardware are observed, never scripted).
std::vector<PhaseSpec> DefaultRealScript(std::uint64_t focused_frames);

// Per-eye expected pixel colors for the current renderEye recipe, so the
// test can verify readback bytes. RGBA, 8-bit per channel.
struct EyeColors {
  std::uint8_t clear_rgba[4];
  std::uint8_t quad_rgba[4];
};

class SceneRenderer {
 public:
  SceneRenderer();
  ~SceneRenderer();
  SceneRenderer(const SceneRenderer&) = delete;
  SceneRenderer& operator=(const SceneRenderer&) = delete;

  bool startup(std::uint32_t width, std::uint32_t height,
               std::string* error);
  void shutdown();
  bool ready() const;
  std::uint32_t width() const;
  std::uint32_t height() const;

  // Renders one eye (0 = left, 1 = right). Eye-distinguishing tint plus a
  // centered quad whose brightness follows yaw_radians. Returns false on any
  // D3D11 failure or out-of-range eye.
  bool renderEye(std::uint32_t eye, float yaw_radians);

  // Expected colors for (eye, yaw); must match what renderEye drew.
  static EyeColors ExpectedColors(std::uint32_t eye, float yaw_radians);

  // Reads back two probe pixels: near the corner (clear color) and dead
  // center (quad color). Returns false on any D3D11 failure.
  bool readback(std::uint32_t eye, std::uint8_t corner_rgba[4],
                std::uint8_t center_rgba[4]);

 private:
  struct Native;
  Native* native_ = nullptr;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  bool ready_ = false;
};

// Aggregate run record. Written by the worker thread, read after join.
struct SessionStats {
  std::uint64_t rendered = 0;           // endFrame(true) frames
  std::uint64_t unsubmitted = 0;        // endFrame(false) frames
  std::uint64_t gated_absent = 0;       // headset-absent frames (no calls)
  std::uint64_t waited_not_ready = 0;   // should_render=false frames
  std::uint64_t begin_failed = 0;
  std::uint64_t pixels_verified = 0;    // per-eye readbacks checked
  std::uint64_t pixel_failures = 0;
  std::vector<std::string> transitions;  // "IDLE->READY @frame 12"
  std::vector<std::string> shutdown_order;
  std::thread::id worker_id;
  std::thread::id caller_id;
  bool worker_ran = false;
  XrTime first_time_ns = 0;
  XrTime last_time_ns = 0;
  bool monotonic = true;
  bool spaces_ok = true;
  bool fov_asymmetric = false;
  ControllerState sampled_left{};
  ControllerState sampled_right{};
  bool sampled_input = false;
  bool recenter_applied = false;
  std::uint64_t recenter_frame = 0;
  std::string error;
};

class SceneSession {
 public:
  SceneSession(IXrBackend* backend, SceneRenderer* renderer);
  ~SceneSession();

  void setPhases(const std::vector<PhaseSpec>& phases);

  // Main-thread recenter request (real-backend path); the worker applies it
  // inside its loop, mirroring the real backend's request path.
  void requestRecenter();

  // Runs the script on a dedicated XR worker thread and joins it. Does NOT
  // shut the backend down; the caller owns shutdown ordering.
  void run();
  const SessionStats& stats() const;

 private:
  void workerMain();

  IXrBackend* backend_ = nullptr;
  SceneRenderer* renderer_ = nullptr;
  std::vector<PhaseSpec> phases_;
  std::atomic<bool> recenter_requested_{false};
  SessionStats stats_;
};

}  // namespace mecvr::openxr::scene
