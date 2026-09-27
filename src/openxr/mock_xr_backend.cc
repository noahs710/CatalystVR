#include "openxr/mock_xr_backend.h"

#include <cmath>

namespace mecvr::openxr {

void MockXRBackend::configure(const MockConfig& config) {
  config_ = config;
  raw_head_ = config_.hmd_pose;
  predicted_time_ns_ = config_.start_time_ns;
  controllers_[0].grip_pose = config_.left_controller_pose;
  controllers_[0].pose_valid = true;
  controllers_[0].buttons = 0u;
  controllers_[1].grip_pose = config_.right_controller_pose;
  controllers_[1].pose_valid = true;
  controllers_[1].buttons = 0u;
}

void MockXRBackend::setHmdPose(const XrPosef& pose) {
  config_.hmd_pose = pose;
  if (config_.trajectory == TrajectoryMode::kStatic && !replaying_) {
    raw_head_ = pose;
  }
}

void MockXRBackend::injectControllerState(Hand hand,
                                          const ControllerState& state) {
  controllerFor(hand) = state;
}

void MockXRBackend::setControllerPose(Hand hand, const XrPosef& pose,
                                      bool valid) {
  controllerFor(hand).grip_pose = pose;
  controllerFor(hand).pose_valid = valid;
}

void MockXRBackend::setControllerButtons(Hand hand, std::uint32_t buttons) {
  controllerFor(hand).buttons = buttons;
}

void MockXRBackend::setTrajectoryMode(TrajectoryMode mode) {
  config_.trajectory = mode;
}

void MockXRBackend::setStageOrigin(const XrPosef& origin) {
  config_.stage_origin = origin;
}

void MockXRBackend::beginRecording() {
  recording_ = true;
  recorded_.clear();
}

std::vector<TimestampedPose> MockXRBackend::stopRecording() {
  recording_ = false;
  return recorded_;
}

bool MockXRBackend::recording() const { return recording_; }

void MockXRBackend::startReplay(const std::vector<TimestampedPose>& points) {
  replay_points_ = points;
  replay_index_ = 0;
  replaying_ = true;
}

void MockXRBackend::stopReplay() {
  replaying_ = false;
  replay_points_.clear();
  replay_index_ = 0;
}

bool MockXRBackend::replaying() const { return replaying_; }

std::uint64_t MockXRBackend::frameIndex() const { return frame_index_; }

FrameTiming MockXRBackend::currentTiming() const {
  FrameTiming timing;
  timing.predicted_display_time_ns = predicted_time_ns_;
  timing.predicted_display_period_ns = periodNs();
  timing.frame_index = frame_index_;
  timing.should_render = running_;
  return timing;
}

bool MockXRBackend::startup() {
  running_ = true;
  frame_open_ = false;
  frame_index_ = 0;
  predicted_time_ns_ = config_.start_time_ns;
  raw_head_ = config_.hmd_pose;
  recenter_offset_ = IdentityPose();
  replay_index_ = 0;
  return true;
}

void MockXRBackend::shutdown() {
  running_ = false;
  frame_open_ = false;
  recording_ = false;
  replaying_ = false;
}

bool MockXRBackend::running() const { return running_; }

FrameTiming MockXRBackend::waitFrame() {
  if (running_) {
    ++frame_index_;
    if (replaying_ && replay_index_ < replay_points_.size()) {
      // Exact replay: reproduce the recorded timestamp and pose verbatim.
      predicted_time_ns_ = replay_points_[replay_index_].time_ns;
      raw_head_ = replay_points_[replay_index_].pose;
      ++replay_index_;
    } else if (replaying_ && !replay_points_.empty()) {
      // Exhausted replay: hold the last pose, keep time monotonic.
      raw_head_ = replay_points_.back().pose;
      predicted_time_ns_ += periodNs();
    } else {
      predicted_time_ns_ = config_.start_time_ns +
                           static_cast<XrTime>(frame_index_) * periodNs();
      raw_head_ = trajectoryPose(predicted_time_ns_);
    }
    if (recording_) {
      recorded_.push_back(TimestampedPose{predicted_time_ns_, raw_head_});
    }
  }
  return currentTiming();
}

bool MockXRBackend::beginFrame() {
  if (!running_ || frame_open_ || frame_index_ == 0) return false;
  frame_open_ = true;
  return true;
}

LocatedViews MockXRBackend::locateViews(Space space) {
  LocatedViews out;
  out.space = space;
  out.sample_time_ns = predicted_time_ns_;
  const float half_ipd = config_.ipd_meters * 0.5f;
  const XrVector3f offsets[2] = {XrVector3f{-half_ipd, 0.0f, 0.0f},
                                 XrVector3f{half_ipd, 0.0f, 0.0f}};
  const XrFovf fovs[2] = {config_.left_fov, config_.right_fov};
  for (std::uint32_t eye = 0; eye < 2; ++eye) {
    XrPosef head;
    switch (space) {
      case Space::kView:
        // Head-locked: the head is identity in its own space.
        head = IdentityPose();
        break;
      case Space::kStage:
        // Room-scale origin; recenter does not apply.
        head = ComposePose(config_.stage_origin, raw_head_);
        break;
      case Space::kLocal:
      default:
        head = headPoseLocal();
        break;
    }
    out.views[eye].pose.orientation = head.orientation;
    const XrVector3f turned = Rotate(head.orientation, offsets[eye]);
    out.views[eye].pose.position = XrVector3f{
        head.position.x + turned.x, head.position.y + turned.y,
        head.position.z + turned.z};
    out.views[eye].fov = fovs[eye];
  }
  return out;
}

std::uint32_t MockXRBackend::acquireSwapchainImage(
    std::uint32_t view_index) {
  if (view_index >= 2) return 0u;
  return static_cast<std::uint32_t>(
      (frame_index_ + view_index) % kSwapchainImageCount);
}

void MockXRBackend::releaseSwapchainImage(std::uint32_t view_index) {
  (void)view_index;
}

bool MockXRBackend::endFrame(bool submitted) {
  (void)submitted;
  if (!frame_open_) return false;
  frame_open_ = false;
  return true;
}

void MockXRBackend::recenter() {
  // Re-baseline LOCAL on the current raw head pose. VIEW (head-locked) and
  // STAGE (room-scale) are unaffected.
  recenter_offset_ = raw_head_;
}

ControllerState MockXRBackend::controllerState(Hand hand) {
  return controllerFor(hand);
}

float MockXRBackend::displayFrequencyHz() const {
  return config_.display_frequency_hz;
}

ViewConfig MockXRBackend::viewConfig(std::uint32_t view_index) const {
  ViewConfig config;
  if (view_index < 2) {
    config.recommended_width = config_.recommended_width;
    config.recommended_height = config_.recommended_height;
    config.max_width = config_.max_width;
    config.max_height = config_.max_height;
  }
  return config;
}

std::uint32_t MockXRBackend::viewCount() const { return 2u; }

XrTime MockXRBackend::periodNs() const {
  if (config_.display_frequency_hz <= 0.0f) return kNanosecondsPerSecond / 90;
  const double period =
      static_cast<double>(kNanosecondsPerSecond) /
      static_cast<double>(config_.display_frequency_hz);
  return static_cast<XrTime>(period + 0.5);
}

XrPosef MockXRBackend::trajectoryPose(XrTime time_ns) const {
  constexpr double kTwoPi = 6.28318530717958647692;
  const double t = static_cast<double>(time_ns - config_.start_time_ns) /
                   static_cast<double>(kNanosecondsPerSecond);
  switch (config_.trajectory) {
    case TrajectoryMode::kSineYaw: {
      const double yaw = static_cast<double>(config_.trajectory_amplitude_rad) *
                         std::sin(kTwoPi * static_cast<double>(
                                                  config_.trajectory_frequency_hz) *
                                      t);
      XrPosef out = config_.hmd_pose;
      out.orientation = Multiply(YawQuaternion(static_cast<float>(yaw)),
                                 config_.hmd_pose.orientation);
      return out;
    }
    case TrajectoryMode::kCircle: {
      const double angle = kTwoPi * static_cast<double>(
                                          config_.trajectory_frequency_hz) *
                           t;
      const double r = static_cast<double>(config_.trajectory_radius_m);
      XrPosef out = config_.hmd_pose;
      out.position.x += static_cast<float>(r * (std::cos(angle) - 1.0));
      out.position.z += static_cast<float>(r * std::sin(angle));
      return out;
    }
    case TrajectoryMode::kReplay:
    case TrajectoryMode::kStatic:
    default:
      return config_.hmd_pose;
  }
}

XrPosef MockXRBackend::headPoseLocal() const {
  return RelativePose(recenter_offset_, raw_head_);
}

ControllerState& MockXRBackend::controllerFor(Hand hand) {
  return controllers_[hand == Hand::kLeft ? 0 : 1];
}

bool MockXRBackend::registerSharedCapture(
    const render::SharedCaptureRegistration& registration) {
  if (!running_ || !render::IsValidSharedCaptureRegistration(registration))
    return false;
  shared_registration_ = registration;
  return true;
}

bool MockXRBackend::submitSharedFrame(
    const render::SharedCaptureFrame& frame) {
  if (!running_ || !frame_open_ ||
      !render::IsValidSharedCaptureFrame(frame, shared_registration_)) {
    return false;
  }
  ++shared_frames_submitted_;
  return true;
}

void MockXRBackend::unregisterSharedCapture(std::uint64_t generation) {
  if (generation == 0 || generation == shared_registration_.generation)
    shared_registration_ = {};
}

}  // namespace mecvr::openxr
