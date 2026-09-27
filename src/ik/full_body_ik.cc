#include "ik/full_body_ik.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <utility>

namespace mecvr::ik {
namespace {

constexpr double kEpsilon = 1e-9;
constexpr double kPi = 3.14159265358979323846;

Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Scale(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}
double Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
Vec3 Normalize(Vec3 a, Vec3 fallback) {
  const double length = Length(a);
  return length > kEpsilon ? Scale(a, 1.0 / length) : fallback;
}

bool Finite(double value) { return std::isfinite(value); }

bool Finite(Vec3 value) {
  return Finite(value.x) && Finite(value.y) && Finite(value.z);
}

bool Finite(Quat value) {
  return Finite(value.x) && Finite(value.y) && Finite(value.z) &&
         Finite(value.w);
}

bool UsablePose(const TrackedPose& pose) {
  if (!pose.valid) return false;
  if (!Finite(pose.position) || !Finite(pose.orientation)) return false;
  const double norm = std::sqrt(
      pose.orientation.x * pose.orientation.x +
      pose.orientation.y * pose.orientation.y +
      pose.orientation.z * pose.orientation.z +
      pose.orientation.w * pose.orientation.w);
  return norm > kEpsilon;
}

Quat Normalize(Quat q) {
  const double length =
      std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (length <= kEpsilon) return {};
  return {q.x / length, q.y / length, q.z / length, q.w / length};
}

Vec3 Rotate(Quat q, Vec3 value) {
  q = Normalize(q);
  const Vec3 u{q.x, q.y, q.z};
  const Vec3 uv = Cross(u, value);
  const Vec3 uuv = Cross(u, uv);
  return Add(value, Add(Scale(uv, 2.0 * q.w), Scale(uuv, 2.0)));
}

Quat FromZ(Vec3 direction) {
  const Vec3 z = Normalize(direction, {0.0, 0.0, 1.0});
  const double d = std::clamp(z.z, -1.0, 1.0);
  if (d > 1.0 - kEpsilon) return {};
  if (d < -1.0 + kEpsilon) return {0.0, 1.0, 0.0, 0.0};
  const Vec3 axis = Cross({0.0, 0.0, 1.0}, z);
  const double s = std::sqrt((1.0 + d) * 2.0);
  return Normalize({axis.x / s, axis.y / s, axis.z / s, s * 0.5});
}

TrackedPose Point(Vec3 position, Quat orientation = {}) {
  return {position, orientation, true};
}

TrackedPose Segment(Vec3 from, Vec3 to) {
  return Point(from, FromZ(Sub(to, from)));
}

void Set(HumanoidPoseFrame* frame, BodyJoint joint, TrackedPose pose) {
  frame->joints[static_cast<std::size_t>(joint)] = pose;
}

struct LegSolution {
  TrackedPose hip{};
  TrackedPose knee{};
  TrackedPose ankle{};
  TrackedPose foot{};
};

LegSolution SolveLeg(Vec3 hip, Vec3 ankle_target, Vec3 knee_pole,
                     Vec3 forward, double thigh, double shin, double foot) {
  LegSolution result;
  Vec3 delta = Sub(ankle_target, hip);
  const double raw_distance = Length(delta);
  const double maximum = std::max(kEpsilon, thigh + shin - 1e-6);
  const double minimum = std::max(1e-6, std::fabs(thigh - shin) + 1e-6);
  const double distance = std::clamp(raw_distance, minimum, maximum);
  const Vec3 direction = Normalize(delta, {0.0, -1.0, 0.0});
  const Vec3 ankle = Add(hip, Scale(direction, distance));
  Vec3 pole = Sub(knee_pole, hip);
  pole = Sub(pole, Scale(direction, Dot(pole, direction)));
  pole = Normalize(pole, forward);
  const double along =
      (thigh * thigh + distance * distance - shin * shin) /
      (2.0 * distance);
  const double height =
      std::sqrt(std::max(0.0, thigh * thigh - along * along));
  const Vec3 knee = Add(Add(hip, Scale(direction, along)), Scale(pole, height));
  const Vec3 toe = Add(ankle, Scale(forward, foot));
  result.hip = Segment(hip, knee);
  result.knee = Segment(knee, ankle);
  result.ankle = Segment(ankle, toe);
  result.foot = Point(toe, FromZ(forward));
  return result;
}

}  // namespace

FullBodyAnimator::FullBodyAnimator(BodyCalibration calibration)
    : calibration_(calibration) {}

void FullBodyAnimator::reset() {
  gait_phase_ = 0.0;
  previous_triggers_ = {};
  recoil_ = {};
}

HumanoidPoseFrame FullBodyAnimator::update(const FullBodyInput& input) {
  HumanoidPoseFrame frame;
  frame.sequence = input.sequence;
  frame.sample_time_ns = input.sample_time_ns;
  if (!UsablePose(input.head)) return frame;

  // Treat malformed controller samples as tracking loss for that limb while
  // retaining the procedural fallback. This is important for both optional
  // OpenXR body providers and future Catalyst-native adapters: a provider's
  // valid bit is not enough to make a transform safe to consume.
  FullBodyInput safe_input = input;
  if (!Finite(safe_input.floor_origin)) safe_input.floor_origin = {};
  if (!Finite(safe_input.velocity)) safe_input.velocity = {};
  if (!Finite(safe_input.delta_seconds)) safe_input.delta_seconds = 0.0;
  if (!UsablePose(safe_input.left_hand.pose))
    safe_input.left_hand.pose.valid = false;
  if (!UsablePose(safe_input.right_hand.pose))
    safe_input.right_hand.pose.valid = false;

  frame.trigger_values = {std::clamp(safe_input.left_hand.trigger, 0.0f, 1.0f),
                          std::clamp(safe_input.right_hand.trigger, 0.0f, 1.0f)};
  frame.grip_values = {std::clamp(safe_input.left_hand.grip, 0.0f, 1.0f),
                       std::clamp(safe_input.right_hand.grip, 0.0f, 1.0f)};
  for (std::size_t hand = 0; hand < 2; ++hand) {
    frame.shot_pulses[hand] =
        frame.trigger_values[hand] > 0.55f && previous_triggers_[hand] <= 0.55f;
    previous_triggers_[hand] = frame.trigger_values[hand];
  }
  const double dt = std::clamp(safe_input.delta_seconds, 0.0, 0.1);
  for (std::size_t hand = 0; hand < 2; ++hand) {
    if (frame.shot_pulses[hand]) {
      recoil_[hand] = std::min(0.055, recoil_[hand] + 0.055);
    } else {
      recoil_[hand] = std::max(0.0, recoil_[hand] - dt * 0.75);
    }
  }

  frame.source_flags |= kSourceHead;
  if (safe_input.left_hand.pose.valid) frame.source_flags |= kSourceLeftHand;
  if (safe_input.right_hand.pose.valid) frame.source_flags |= kSourceRightHand;
  if (safe_input.grounded) frame.source_flags |= kSourceGrounded;

  Vec3 forward = Rotate(safe_input.head.orientation, {0.0, 0.0, -1.0});
  forward.y = 0.0;
  forward = Normalize(forward, {0.0, 0.0, -1.0});
  const Vec3 right = Normalize(Cross(forward, {0.0, 1.0, 0.0}),
                               {1.0, 0.0, 0.0});
  const double speed =
      std::sqrt(safe_input.velocity.x * safe_input.velocity.x +
                safe_input.velocity.z * safe_input.velocity.z);
  const double head_height =
      safe_input.head.position.y - safe_input.floor_origin.y;
  if (safe_input.climbing) {
    frame.state = BodyAnimationState::kClimb;
  } else if (safe_input.vaulting) {
    frame.state = BodyAnimationState::kVault;
  } else if (safe_input.wall_running) {
    frame.state = BodyAnimationState::kWallRun;
  } else if (!safe_input.grounded) {
    frame.state = BodyAnimationState::kAirborne;
  } else if (safe_input.sliding) {
    frame.state = BodyAnimationState::kSlide;
  } else if (head_height < calibration_.standing_height * 0.78) {
    frame.state = BodyAnimationState::kCrouch;
  } else if (speed > 0.12) {
    frame.state = BodyAnimationState::kLocomotion;
  }

  if (frame.state == BodyAnimationState::kLocomotion) {
    gait_phase_ = std::fmod(gait_phase_ + dt * (4.0 + speed * 3.0),
                            2.0 * kPi);
  }
  frame.gait_phase = gait_phase_;

  const double pelvis_floor = calibration_.thigh + calibration_.shin;
  const double pelvis_y = std::clamp(
      safe_input.head.position.y - calibration_.eye_to_pelvis,
      safe_input.floor_origin.y + pelvis_floor * 0.70,
      safe_input.floor_origin.y + pelvis_floor * 1.05);
  const Vec3 pelvis{safe_input.head.position.x, pelvis_y,
                    safe_input.head.position.z};
  const Vec3 root{pelvis.x, safe_input.floor_origin.y, pelvis.z};
  const double lean = frame.state == BodyAnimationState::kVault
                           ? 0.18
                           : frame.state == BodyAnimationState::kSlide ? 0.10 : 0.0;
  const Vec3 chest = Add(
      pelvis, Add({0.0, (safe_input.head.position.y - pelvis.y) * 0.58, 0.0},
                  Scale(forward, lean)));
  const Vec3 neck = Add(
      pelvis, Add({0.0, (safe_input.head.position.y - pelvis.y) * 0.84, 0.0},
                  Scale(forward, lean * 1.25)));
  Set(&frame, BodyJoint::kRoot, Point(root, safe_input.head.orientation));
  Set(&frame, BodyJoint::kPelvis, Segment(pelvis, chest));
  Set(&frame, BodyJoint::kChest, Segment(chest, neck));
  Set(&frame, BodyJoint::kNeck, Segment(neck, safe_input.head.position));
  Set(&frame, BodyJoint::kHead, safe_input.head);

  const Vec3 left_shoulder =
      Add(chest, Scale(right, -calibration_.shoulder_width * 0.5));
  const Vec3 right_shoulder =
      Add(chest, Scale(right, calibration_.shoulder_width * 0.5));
  MotionHand left = safe_input.left_hand;
  MotionHand right_hand = safe_input.right_hand;
  if (!left.pose.valid) {
    left.pose = Point(Add(left_shoulder, Add(Scale(right, -0.18),
                                             Scale(forward, 0.16))));
  }
  if (!right_hand.pose.valid) {
    right_hand.pose = Point(Add(right_shoulder, Add(Scale(right, 0.18),
                                                    Scale(forward, 0.16))));
  }
  ArmRig left_rig{left_shoulder,
                  Add(left_shoulder, Add(Scale(right, -0.15),
                                         Scale(forward, 0.32))),
                  calibration_.upper_arm, calibration_.forearm,
                  calibration_.hand};
  ArmRig right_rig{right_shoulder,
                   Add(right_shoulder, Add(Scale(right, 0.15),
                                           Scale(forward, 0.32))),
                   calibration_.upper_arm, calibration_.forearm,
                   calibration_.hand};
  const ArmPose left_arm = SolveArm(left_rig, {left.pose, left_rig.pole, 1.0});
  const ArmPose right_arm =
      SolveArm(right_rig, {right_hand.pose, right_rig.pole, 1.0});
  Set(&frame, BodyJoint::kLeftShoulder, left_arm.shoulder);
  Set(&frame, BodyJoint::kLeftElbow, left_arm.elbow);
  Set(&frame, BodyJoint::kLeftWrist, left_arm.wrist);
  Set(&frame, BodyJoint::kLeftHand, left_arm.hand);
  Set(&frame, BodyJoint::kRightShoulder, right_arm.shoulder);
  Set(&frame, BodyJoint::kRightElbow, right_arm.elbow);
  Set(&frame, BodyJoint::kRightWrist, right_arm.wrist);
  Set(&frame, BodyJoint::kRightHand, right_arm.hand);

  // Mod-owned firing animation: preserve controller-driven aim while adding a
  // short, deterministic rearward impulse to the solved arm chain. The pulse
  // is intentionally separate from Catalyst's private weapon animation so it
  // remains replayable and can later be handed to a native skeleton adapter.
  const auto apply_recoil = [&](std::size_t hand, BodyJoint elbow,
                                BodyJoint wrist, BodyJoint hand_joint,
                                const MotionHand& source) {
    if (recoil_[hand] <= 0.0) return;
    Vec3 direction = Rotate(source.pose.orientation, {0.0, 0.0, 1.0});
    direction = Normalize(direction, forward);
    const double amount = recoil_[hand];
    frame.joints[static_cast<std::size_t>(elbow)].position = Add(
        frame.joints[static_cast<std::size_t>(elbow)].position,
        Scale(direction, amount * 0.35));
    frame.joints[static_cast<std::size_t>(wrist)].position = Add(
        frame.joints[static_cast<std::size_t>(wrist)].position,
        Scale(direction, amount * 0.70));
    frame.joints[static_cast<std::size_t>(hand_joint)].position = Add(
        frame.joints[static_cast<std::size_t>(hand_joint)].position,
        Scale(direction, amount));
  };
  apply_recoil(0, BodyJoint::kLeftElbow, BodyJoint::kLeftWrist,
               BodyJoint::kLeftHand, left);
  apply_recoil(1, BodyJoint::kRightElbow, BodyJoint::kRightWrist,
               BodyJoint::kRightHand, right_hand);

  Vec3 travel = safe_input.velocity;
  travel.y = 0.0;
  travel = Normalize(travel, forward);
  const double stride = frame.state == BodyAnimationState::kLocomotion
                            ? std::clamp(speed * 0.14, 0.05, 0.28)
                            : 0.0;
  const double left_wave = std::sin(gait_phase_);
  const double right_wave = -left_wave;
  const double floor_y = safe_input.floor_origin.y + 0.04;
  const Vec3 left_hip = Add(pelvis, Scale(right, -calibration_.hip_width * 0.5));
  const Vec3 right_hip = Add(pelvis, Scale(right, calibration_.hip_width * 0.5));
  Vec3 left_ankle = Add(safe_input.floor_origin,
                        Add(Scale(right, -calibration_.hip_width * 0.5),
                            Scale(travel, left_wave * stride)));
  Vec3 right_ankle = Add(safe_input.floor_origin,
                         Add(Scale(right, calibration_.hip_width * 0.5),
                             Scale(travel, right_wave * stride)));
  left_ankle.y = floor_y + std::max(0.0, left_wave) * 0.12;
  right_ankle.y = floor_y + std::max(0.0, right_wave) * 0.12;
  if (frame.state == BodyAnimationState::kAirborne) {
    left_ankle.y = right_ankle.y = pelvis.y - pelvis_floor * 0.88;
  } else if (frame.state == BodyAnimationState::kVault) {
    left_ankle.y += 0.10;
    right_ankle.y += 0.10;
  } else if (frame.state == BodyAnimationState::kSlide) {
    left_ankle.y = right_ankle.y = floor_y;
  }
  const LegSolution left_leg = SolveLeg(
      left_hip, left_ankle,
      Add(left_hip, Add(Scale(right, -0.08), Scale(forward, 0.35))), forward,
      calibration_.thigh, calibration_.shin, calibration_.foot);
  const LegSolution right_leg = SolveLeg(
      right_hip, right_ankle,
      Add(right_hip, Add(Scale(right, 0.08), Scale(forward, 0.35))), forward,
      calibration_.thigh, calibration_.shin, calibration_.foot);
  Set(&frame, BodyJoint::kLeftHip, left_leg.hip);
  Set(&frame, BodyJoint::kLeftKnee, left_leg.knee);
  Set(&frame, BodyJoint::kLeftAnkle, left_leg.ankle);
  Set(&frame, BodyJoint::kLeftFoot, left_leg.foot);
  Set(&frame, BodyJoint::kRightHip, right_leg.hip);
  Set(&frame, BodyJoint::kRightKnee, right_leg.knee);
  Set(&frame, BodyJoint::kRightAnkle, right_leg.ankle);
  Set(&frame, BodyJoint::kRightFoot, right_leg.foot);
  if (safe_input.has_tracked_body) {
    bool any_tracked = false;
    for (std::size_t i = 0; i < safe_input.tracked_joints.size(); ++i) {
      if (!UsablePose(safe_input.tracked_joints[i])) continue;
      frame.joints[i] = safe_input.tracked_joints[i];
      any_tracked = true;
    }
    if (any_tracked) frame.source_flags |= kSourceFullBody;
  }
  frame.valid = left_arm.valid && right_arm.valid;
  return frame;
}

void BodyPoseMailbox::publish(const HumanoidPoseFrame& frame) {
  if (!frame.valid) return;
  std::lock_guard<std::mutex> lock(mutex_);
  frame_ = frame;
  ++sequence_;
}

bool BodyPoseMailbox::latest(HumanoidPoseFrame* out) const {
  if (out == nullptr) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (sequence_ == 0 || !frame_.valid) return false;
  *out = frame_;
  return true;
}

MotionClip::MotionClip(std::size_t max_frames)
    : max_frames_(std::clamp<std::size_t>(max_frames, 1, 1800)) {}

bool MotionClip::append(const HumanoidPoseFrame& frame) {
  if (!frame.valid || frames_.size() >= max_frames_) return false;
  if (!frames_.empty() && frame.sample_time_ns < frames_.back().sample_time_ns)
    return false;
  frames_.push_back(frame);
  return true;
}

void MotionClip::clear() { frames_.clear(); }

double MotionClip::durationSeconds() const {
  if (frames_.size() < 2) return 0.0;
  const auto span = frames_.back().sample_time_ns - frames_.front().sample_time_ns;
  return std::max(0.0, static_cast<double>(span) / 1.0e9);
}

bool MotionClip::sample(double elapsed_seconds, HumanoidPoseFrame* out) const {
  if (out == nullptr || frames_.empty() || !std::isfinite(elapsed_seconds))
    return false;
  const auto& first = frames_.front();
  const auto& last = frames_.back();
  const std::int64_t first_ns = first.sample_time_ns;
  const std::int64_t last_ns = last.sample_time_ns;
  const double requested_ns =
      std::max(0.0, elapsed_seconds) * 1.0e9 + static_cast<double>(first_ns);
  if (requested_ns <= static_cast<double>(first_ns)) {
    *out = first;
    return true;
  }
  if (requested_ns >= static_cast<double>(last_ns)) {
    *out = last;
    return true;
  }
  const auto upper = std::upper_bound(
      frames_.begin(), frames_.end(), requested_ns,
      [](double time, const HumanoidPoseFrame& frame) {
        return time < static_cast<double>(frame.sample_time_ns);
      });
  if (upper == frames_.begin() || upper == frames_.end()) return false;
  const auto& b = *upper;
  const auto& a = *(upper - 1);
  const double span = static_cast<double>(b.sample_time_ns - a.sample_time_ns);
  const double t = span > 0.0
                       ? std::clamp((requested_ns - a.sample_time_ns) / span,
                                    0.0, 1.0)
                       : 0.0;
  *out = a;
  out->sequence = b.sequence;
  out->sample_time_ns = static_cast<std::int64_t>(requested_ns);
  out->gait_phase = a.gait_phase + (b.gait_phase - a.gait_phase) * t;
  for (std::size_t hand = 0; hand < 2; ++hand) {
    out->trigger_values[hand] = static_cast<float>(
        a.trigger_values[hand] + (b.trigger_values[hand] - a.trigger_values[hand]) * t);
    out->grip_values[hand] = static_cast<float>(
        a.grip_values[hand] + (b.grip_values[hand] - a.grip_values[hand]) * t);
    out->shot_pulses[hand] = a.shot_pulses[hand];
  }
  for (std::size_t i = 0; i < out->joints.size(); ++i) {
    const auto& ap = a.joints[i];
    const auto& bp = b.joints[i];
    if (!ap.valid || !bp.valid) {
      out->joints[i] = t < 0.5 ? ap : bp;
      continue;
    }
    auto& p = out->joints[i];
    p.valid = true;
    p.position.x = ap.position.x + (bp.position.x - ap.position.x) * t;
    p.position.y = ap.position.y + (bp.position.y - ap.position.y) * t;
    p.position.z = ap.position.z + (bp.position.z - ap.position.z) * t;
    Quat q{ap.orientation.x + (bp.orientation.x - ap.orientation.x) * t,
           ap.orientation.y + (bp.orientation.y - ap.orientation.y) * t,
           ap.orientation.z + (bp.orientation.z - ap.orientation.z) * t,
           ap.orientation.w + (bp.orientation.w - ap.orientation.w) * t};
    p.orientation = Normalize(q);
  }
  out->valid = true;
  return true;
}

namespace {
constexpr std::uint32_t kMotionClipMagic = 0x4D454356u;  // "MECV"
constexpr std::uint32_t kMotionClipVersion = 1u;

template <typename T>
bool Write(std::ofstream& stream, const T& value) {
  stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
  return stream.good();
}

template <typename T>
bool Read(std::ifstream& stream, T* value) {
  stream.read(reinterpret_cast<char*>(value), sizeof(*value));
  return stream.good();
}

bool WriteFrame(std::ofstream& stream, const HumanoidPoseFrame& frame) {
  const std::uint8_t state = static_cast<std::uint8_t>(frame.state);
  const std::uint8_t valid = frame.valid ? 1 : 0;
  if (!Write(stream, frame.sequence) || !Write(stream, frame.sample_time_ns) ||
      !Write(stream, state) || !Write(stream, frame.source_flags) ||
      !Write(stream, frame.gait_phase) || !Write(stream, valid)) return false;
  for (std::size_t i = 0; i < 2; ++i)
    if (!Write(stream, frame.trigger_values[i]) ||
        !Write(stream, frame.grip_values[i])) return false;
  for (std::size_t i = 0; i < frame.shot_pulses.size(); ++i) {
    const std::uint8_t pulse = frame.shot_pulses[i] ? 1 : 0;
    if (!Write(stream, pulse)) return false;
  }
  for (const auto& joint : frame.joints) {
    const std::uint8_t joint_valid = joint.valid ? 1 : 0;
    if (!Write(stream, joint.position.x) || !Write(stream, joint.position.y) ||
        !Write(stream, joint.position.z) || !Write(stream, joint.orientation.x) ||
        !Write(stream, joint.orientation.y) || !Write(stream, joint.orientation.z) ||
        !Write(stream, joint.orientation.w) || !Write(stream, joint_valid)) return false;
  }
  return true;
}

bool ReadFrame(std::ifstream& stream, HumanoidPoseFrame* frame) {
  std::uint8_t state = 0, valid = 0;
  if (!Read(stream, &frame->sequence) || !Read(stream, &frame->sample_time_ns) ||
      !Read(stream, &state) || !Read(stream, &frame->source_flags) ||
      !Read(stream, &frame->gait_phase) || !Read(stream, &valid)) return false;
  if (state > static_cast<std::uint8_t>(BodyAnimationState::kWallRun)) return false;
  frame->state = static_cast<BodyAnimationState>(state);
  frame->valid = valid != 0;
  for (std::size_t i = 0; i < 2; ++i)
    if (!Read(stream, &frame->trigger_values[i]) ||
        !Read(stream, &frame->grip_values[i])) return false;
  for (std::size_t i = 0; i < frame->shot_pulses.size(); ++i) {
    std::uint8_t pulse = 0;
    if (!Read(stream, &pulse)) return false;
    frame->shot_pulses[i] = pulse != 0;
  }
  for (auto& joint : frame->joints) {
    std::uint8_t joint_valid = 0;
    if (!Read(stream, &joint.position.x) || !Read(stream, &joint.position.y) ||
        !Read(stream, &joint.position.z) || !Read(stream, &joint.orientation.x) ||
        !Read(stream, &joint.orientation.y) || !Read(stream, &joint.orientation.z) ||
        !Read(stream, &joint.orientation.w) || !Read(stream, &joint_valid)) return false;
    joint.valid = joint_valid != 0;
    if (!Finite(joint.position) || !Finite(joint.orientation)) return false;
  }
  return frame->valid;
}
}  // namespace

bool MotionClip::save(const std::string& path) const {
  if (path.empty() || frames_.empty() || frames_.size() > 1800) return false;
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) return false;
  const std::uint32_t count = static_cast<std::uint32_t>(frames_.size());
  if (!Write(stream, kMotionClipMagic) || !Write(stream, kMotionClipVersion) ||
      !Write(stream, count)) return false;
  for (const auto& frame : frames_)
    if (!WriteFrame(stream, frame)) return false;
  return stream.good();
}

bool MotionClip::load(const std::string& path, MotionClip* out) {
  if (out == nullptr || path.empty()) return false;
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return false;
  std::uint32_t magic = 0, version = 0, count = 0;
  if (!Read(stream, &magic) || !Read(stream, &version) || !Read(stream, &count) ||
      magic != kMotionClipMagic || version != kMotionClipVersion || count == 0 ||
      count > 1800) return false;
  MotionClip parsed(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    HumanoidPoseFrame frame;
    if (!ReadFrame(stream, &frame) || !parsed.append(frame)) return false;
  }
  *out = std::move(parsed);
  return true;
}

}  // namespace mecvr::ik
