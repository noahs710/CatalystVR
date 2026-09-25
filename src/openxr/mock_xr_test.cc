// Deterministic MockXR validation exe (plan T5 validation).
//
// Proves: (1) trajectory replay is bit-exact across runs, (2) frame timing
// advances monotonically, (3) recenter re-baselines LOCAL, (4) controller
// pose/button injection round-trips. Prints PASS/FAIL per check; exit code
// is 0 only when every check passes. No headset, no OpenXR linkage.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "openxr/mock_xr_backend.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++g_failures;
}

bool Near(float a, float b) {
  return std::fabs(a - b) <= 0.00001f;
}

bool PoseNear(const mecvr::openxr::XrPosef& a,
              const mecvr::openxr::XrPosef& b) {
  return Near(a.orientation.x, b.orientation.x) &&
         Near(a.orientation.y, b.orientation.y) &&
         Near(a.orientation.z, b.orientation.z) &&
         Near(a.orientation.w, b.orientation.w) &&
         Near(a.position.x, b.position.x) && Near(a.position.y, b.position.y) &&
         Near(a.position.z, b.position.z);
}

using mecvr::openxr::Hand;
using mecvr::openxr::MockConfig;
using mecvr::openxr::MockXRBackend;
using mecvr::openxr::Space;
using mecvr::openxr::TimestampedPose;
using mecvr::openxr::TrajectoryMode;

// Runs N frames of wait/begin/locate/end, collecting left-eye LOCAL poses.
std::vector<TimestampedPose> RunFrames(MockXRBackend& backend, int frames) {
  std::vector<TimestampedPose> out;
  for (int i = 0; i < frames; ++i) {
    auto timing = backend.waitFrame();
    if (!backend.beginFrame()) break;
    auto views = backend.locateViews(Space::kLocal);
    backend.acquireSwapchainImage(0u);
    backend.acquireSwapchainImage(1u);
    backend.releaseSwapchainImage(0u);
    backend.releaseSwapchainImage(1u);
    if (!backend.endFrame(true)) break;
    out.push_back(TimestampedPose{timing.predicted_display_time_ns,
                                  views.views[0].pose});
  }
  return out;
}

void TestReplayBitExact() {
  constexpr int kFrames = 120;
  MockConfig config;
  config.trajectory = TrajectoryMode::kSineYaw;
  config.trajectory_amplitude_rad = 0.5f;
  config.trajectory_frequency_hz = 0.25f;

  MockXRBackend sim;
  sim.configure(config);
  sim.startup();
  sim.beginRecording();
  for (int i = 0; i < kFrames; ++i) sim.waitFrame();
  const std::vector<TimestampedPose> recording = sim.stopRecording();
  sim.shutdown();
  Check(recording.size() == static_cast<std::size_t>(kFrames),
        "replay: recording holds one pose per frame");

  // Two backends replay the same array; a third pass replays it again.
  MockXRBackend a;
  MockXRBackend b;
  a.configure(config);
  b.configure(config);
  a.startup();
  b.startup();
  a.startReplay(recording);
  b.startReplay(recording);
  const std::vector<TimestampedPose> run_a = RunFrames(a, kFrames);
  const std::vector<TimestampedPose> run_b = RunFrames(b, kFrames);
  a.startReplay(recording);
  const std::vector<TimestampedPose> run_a2 = RunFrames(a, kFrames);

  bool exact = run_a.size() == recording.size() &&
               run_b.size() == recording.size() &&
               run_a2.size() == recording.size();
  // Left-eye LOCAL pose with no recenter is the raw pose shifted by -ipd/2
  // in x, so compare runs against each other bit-exactly and against the
  // recording up to the known rigid eye offset.
  for (std::size_t i = 0; exact && i < recording.size(); ++i) {
    exact = run_a[i].pose == run_b[i].pose &&
            run_a[i].pose == run_a2[i].pose &&
            run_a[i].time_ns == recording[i].time_ns &&
            run_b[i].time_ns == recording[i].time_ns;
  }
  Check(exact, "replay: bit-exact across backends and runs");

  // Replayed head motion matches the source trajectory: the left-eye pose
  // relative to the recorded raw head pose is a pure -ipd/2 x-shift in head
  // space (the eye offset rotates with yaw, so compare relatively).
  bool matches_source = true;
  for (std::size_t i = 0; matches_source && i < recording.size(); ++i) {
    const mecvr::openxr::XrPosef rel =
        mecvr::openxr::RelativePose(recording[i].pose, run_a[i].pose);
    matches_source =
        Near(rel.position.x, -0.032f) && Near(rel.position.y, 0.0f) &&
        Near(rel.position.z, 0.0f) &&
        run_a[i].pose.orientation == recording[i].pose.orientation;
  }
  Check(matches_source, "replay: poses reproduce recorded trajectory");
  a.shutdown();
  b.shutdown();
}

void TestTimingMonotonic() {
  MockXRBackend backend;
  backend.configure(MockConfig());
  backend.startup();
  constexpr int kFrames = 500;
  bool monotonic = true;
  bool constant_period = true;
  mecvr::openxr::XrTime prev = 0;
  mecvr::openxr::XrTime period = 0;
  for (int i = 0; i < kFrames; ++i) {
    const auto timing = backend.waitFrame();
    if (i == 0) {
      period = timing.predicted_display_period_ns;
      monotonic = timing.predicted_display_time_ns > 0 &&
                  timing.frame_index == 1u;
    } else {
      monotonic = monotonic && timing.predicted_display_time_ns > prev;
      constant_period =
          constant_period && timing.predicted_display_time_ns - prev == period;
    }
    prev = timing.predicted_display_time_ns;
  }
  backend.shutdown();
  Check(monotonic, "timing: predicted display time advances monotonically");
  Check(constant_period, "timing: frame interval is a constant period");
  Check(period == 11111111LL, "timing: 90 Hz period is 11111111 ns");
}

void TestRecenterLocal() {
  MockXRBackend backend;
  backend.configure(MockConfig());
  backend.startup();

  mecvr::openxr::XrPosef displaced;
  displaced.orientation = mecvr::openxr::YawQuaternion(0.5f);
  displaced.position = mecvr::openxr::XrVector3f{1.0f, 2.0f, 3.0f};
  backend.setHmdPose(displaced);
  backend.waitFrame();

  // Pre-recenter: LOCAL reflects the displaced head pose (eye-shifted).
  auto before = backend.locateViews(Space::kLocal);
  const mecvr::openxr::XrPosef kIdentity = mecvr::openxr::IdentityPose();
  bool displaced_seen =
      !PoseNear(before.views[0].pose, kIdentity) &&
      !PoseNear(before.views[1].pose, kIdentity);
  Check(displaced_seen, "recenter: LOCAL reflects head pose before recenter");

  backend.recenter();
  backend.waitFrame();
  auto after = backend.locateViews(Space::kLocal);
  // Post-recenter with no motion: head is identity, eyes sit at +/-ipd/2.
  mecvr::openxr::XrPosef want_left = kIdentity;
  want_left.position.x = -0.032f;
  mecvr::openxr::XrPosef want_right = kIdentity;
  want_right.position.x = 0.032f;
  Check(PoseNear(after.views[0].pose, want_left) &&
            PoseNear(after.views[1].pose, want_right),
        "recenter: re-baselines LOCAL to identity head pose");

  // VIEW (head-locked) and STAGE (room-scale) are unaffected by recenter.
  auto view = backend.locateViews(Space::kView);
  Check(PoseNear(view.views[0].pose, want_left) &&
            PoseNear(view.views[1].pose, want_right),
        "recenter: VIEW space unaffected");
  backend.shutdown();
}

void TestControllerRoundTrip() {
  using mecvr::openxr::ControllerState;
  MockXRBackend backend;
  backend.configure(MockConfig());
  backend.startup();

  ControllerState left;
  left.grip_pose.orientation = mecvr::openxr::YawQuaternion(-0.3f);
  left.grip_pose.position = mecvr::openxr::XrVector3f{-0.2f, 1.1f, -0.4f};
  left.pose_valid = true;
  left.buttons = mecvr::openxr::kButtonTrigger |
                 mecvr::openxr::kButtonPrimary |
                 mecvr::openxr::kButtonMenu;
  ControllerState right;
  right.grip_pose.orientation = mecvr::openxr::YawQuaternion(0.7f);
  right.grip_pose.position = mecvr::openxr::XrVector3f{0.25f, 1.3f, -0.35f};
  right.pose_valid = false;
  right.buttons = 0xFFFFFFFFu;
  backend.injectControllerState(Hand::kLeft, left);
  backend.injectControllerState(Hand::kRight, right);

  const ControllerState got_left = backend.controllerState(Hand::kLeft);
  const ControllerState got_right = backend.controllerState(Hand::kRight);
  Check(got_left.grip_pose == left.grip_pose &&
            got_left.pose_valid == left.pose_valid &&
            got_left.buttons == left.buttons,
        "input: left controller pose/button injection round-trips");
  Check(got_right.grip_pose == right.grip_pose &&
            got_right.pose_valid == right.pose_valid &&
            got_right.buttons == right.buttons,
        "input: right controller pose/button injection round-trips");

  // Convenience setters compose with injection.
  backend.setControllerButtons(Hand::kLeft, 0u);
  backend.setControllerPose(Hand::kLeft, mecvr::openxr::IdentityPose(), true);
  const ControllerState cleared = backend.controllerState(Hand::kLeft);
  Check(cleared.buttons == 0u && cleared.pose_valid &&
            cleared.grip_pose == mecvr::openxr::IdentityPose(),
        "input: pose/button setters compose with injection");
  backend.shutdown();
}

void TestViewsAndConfig() {
  MockXRBackend backend;
  MockConfig config;
  config.display_frequency_hz = 72.0f;
  backend.configure(config);
  backend.startup();
  Check(backend.viewCount() == 2u, "views: two eyes reported");
  const auto left = backend.viewConfig(0u);
  Check(left.recommended_width > 0 && left.recommended_height > 0 &&
            left.recommended_width <= left.max_width &&
            left.recommended_height <= left.max_height,
        "views: recommended resolution nonzero and within max");
  backend.waitFrame();
  const auto views = backend.locateViews(Space::kLocal);
  const bool asymmetric =
      views.views[0].fov.angle_left != views.views[1].fov.angle_left ||
      views.views[0].fov.angle_right != views.views[1].fov.angle_right;
  Check(asymmetric, "views: per-eye FOV is asymmetric");
  Check(backend.displayFrequencyHz() == 72.0f,
        "timing: fake display frequency is configurable");
  const auto timing = backend.currentTiming();
  Check(timing.predicted_display_period_ns == 13888889LL,
        "timing: 72 Hz period is 13888889 ns");

  // Frame-loop sequencing guards.
  const bool first_begin = backend.beginFrame();
  const bool second_begin = backend.beginFrame();
  const bool first_end = backend.endFrame(true);
  const bool second_end = backend.endFrame(true);
  Check(first_begin && !second_begin && first_end && !second_end,
        "frame: begin/end sequencing is guarded");
  Check(backend.viewConfig(7u).recommended_width == 0u,
        "views: out-of-range view index yields empty config");
  Check(backend.acquireSwapchainImage(7u) == 0u,
        "frame: out-of-range swapchain view yields image 0");
  backend.shutdown();
}

}  // namespace

int main() {
  TestReplayBitExact();
  TestTimingMonotonic();
  TestRecenterLocal();
  TestControllerRoundTrip();
  TestViewsAndConfig();
  if (g_failures == 0) {
    std::printf("ALL TESTS PASSED\n");
    return 0;
  }
  std::printf("%d TEST(S) FAILED\n", g_failures);
  return 1;
}
