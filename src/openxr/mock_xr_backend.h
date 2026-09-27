#pragma once

// Deterministic MockXR backend (plan T5 deliverable 2). No OpenXR linkage,
// no headset, no game interaction. Drives the M1C test scene (plan T9)
// through IXrBackend with synthetic timing, poses, and input.

#include <cstdint>
#include <vector>

#include "openxr/xr_backend.h"

namespace mecvr::openxr {

// Head-motion trajectory used by waitFrame to derive the raw HMD pose.
enum class TrajectoryMode {
  kStatic,    // Fixed configurable HMD pose.
  kSineYaw,   // Sinusoidal yaw about the configured pose.
  kCircle,    // Horizontal circle about the configured pose.
  kReplay,    // Exact replay of a recorded TimestampedPose array.
};

// One recorded head sample: frame timestamp plus raw (pre-recenter) pose.
struct TimestampedPose {
  XrTime time_ns = 0;
  XrPosef pose;
};

struct MockConfig {
  XrPosef hmd_pose = IdentityPose();
  XrPosef left_controller_pose = IdentityPose();
  XrPosef right_controller_pose = IdentityPose();
  float display_frequency_hz = 90.0f;
  std::uint32_t recommended_width = 2064u;
  std::uint32_t recommended_height = 2104u;
  std::uint32_t max_width = 2488u;
  std::uint32_t max_height = 2544u;
  // Asymmetric per-eye frustums (radians).
  XrFovf left_fov = XrFovf{-0.8552f, 0.7854f, 0.8378f, -0.8378f};
  XrFovf right_fov = XrFovf{-0.7854f, 0.8552f, 0.8378f, -0.8378f};
  float ipd_meters = 0.064f;
  TrajectoryMode trajectory = TrajectoryMode::kStatic;
  float trajectory_amplitude_rad = 0.5f;
  float trajectory_frequency_hz = 0.25f;
  float trajectory_radius_m = 0.25f;
  XrTime start_time_ns = 0;
  XrPosef stage_origin = IdentityPose();
};

class MockXRBackend final : public IXrBackend {
 public:
  MockXRBackend() = default;

  void configure(const MockConfig& config);

  // Input injection (test/scene use only; the real backend reads hardware).
  void setHmdPose(const XrPosef& pose);
  void injectControllerState(Hand hand, const ControllerState& state);
  void setControllerPose(Hand hand, const XrPosef& pose, bool valid);
  void setControllerButtons(Hand hand, std::uint32_t buttons);

  // Trajectory control.
  void setTrajectoryMode(TrajectoryMode mode);
  void setStageOrigin(const XrPosef& origin);

  // Recording: while active, each waitFrame appends the frame's
  // (timestamp, raw head pose). stopRecording returns the array.
  void beginRecording();
  std::vector<TimestampedPose> stopRecording();
  bool recording() const;

  // Replay: waitFrame consumes the array in order, reproducing recorded
  // timestamps and poses exactly (bit-exact float copies). After the array
  // is exhausted the last pose is held while time keeps period-stepping.
  void startReplay(const std::vector<TimestampedPose>& points);
  void stopReplay();
  bool replaying() const;

  std::uint64_t frameIndex() const;
  FrameTiming currentTiming() const;

  // IXrBackend.
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
  bool registerSharedCapture(
      const render::SharedCaptureRegistration& registration) override;
  bool submitSharedFrame(const render::SharedCaptureFrame& frame) override;
  void unregisterSharedCapture(std::uint64_t generation) override;
  std::uint64_t sharedFramesSubmitted() const {
    return shared_frames_submitted_;
  }

 private:
  static constexpr std::uint32_t kSwapchainImageCount = 3u;

  XrTime periodNs() const;
  XrPosef trajectoryPose(XrTime time_ns) const;
  XrPosef headPoseLocal() const;
  ControllerState& controllerFor(Hand hand);

  MockConfig config_;
  bool running_ = false;
  bool frame_open_ = false;
  std::uint64_t frame_index_ = 0;
  XrTime predicted_time_ns_ = 0;
  XrPosef raw_head_ = IdentityPose();
  XrPosef recenter_offset_ = IdentityPose();
  ControllerState controllers_[2];
  bool recording_ = false;
  std::vector<TimestampedPose> recorded_;
  bool replaying_ = false;
  std::vector<TimestampedPose> replay_points_;
  std::size_t replay_index_ = 0;
  render::SharedCaptureRegistration shared_registration_{};
  std::uint64_t shared_frames_submitted_ = 0;
};

}  // namespace mecvr::openxr
