#include "ik/arm_ik.h"

#include <algorithm>
#include <cmath>

namespace mecvr::ik {
namespace {

constexpr double kEpsilon = 1e-9;

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

Quat Normalize(Quat q) {
  const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (n <= kEpsilon) return {};
  q.x /= n;
  q.y /= n;
  q.z /= n;
  q.w /= n;
  return q;
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

void SetSegmentPose(TrackedPose* out, Vec3 from, Vec3 to) {
  out->position = from;
  out->orientation = FromZ(Sub(to, from));
  out->valid = true;
}

}  // namespace

ArmPose SolveArm(const ArmRig& rig, const ArmTarget& target,
                 std::size_t iterations) {
  ArmPose result;
  if (!target.hand.valid || rig.upper_arm <= kEpsilon ||
      rig.forearm <= kEpsilon || rig.hand <= kEpsilon) {
    return result;
  }

  const double lengths[3] = {rig.upper_arm, rig.forearm, rig.hand};
  const double reach = lengths[0] + lengths[1] + lengths[2];
  const double min_reach = std::max(0.0, lengths[0] - lengths[1] - lengths[2]);
  const Vec3 shoulder = rig.shoulder;
  Vec3 to_target = Sub(target.hand.position, shoulder);
  double distance = Length(to_target);
  const Vec3 forward = Normalize(to_target, {0.0, 0.0, -1.0});
  bool clamped = false;
  if (distance > reach) {
    distance = reach;
    to_target = Scale(forward, distance);
    clamped = true;
  } else if (distance < min_reach && min_reach > kEpsilon) {
    distance = min_reach;
    to_target = Scale(forward, distance);
    clamped = true;
  }
  const Vec3 goal = Add(shoulder, to_target);

  Vec3 pole = Sub(rig.pole, shoulder);
  pole = Sub(pole, Scale(forward, Dot(pole, forward)));
  pole = Normalize(pole, {0.0, 1.0, 0.0});
  Vec3 side = Normalize(Cross(forward, pole), {1.0, 0.0, 0.0});
  pole = Normalize(Cross(side, forward), {0.0, 1.0, 0.0});

  Vec3 points[4];
  points[0] = shoulder;
  points[1] = Add(shoulder, Scale(pole, lengths[0]));
  points[2] = Add(points[1], Scale(forward, lengths[1]));
  points[3] = Add(points[2], Scale(forward, lengths[2]));
  const std::size_t passes = std::max<std::size_t>(1, iterations);
  for (std::size_t pass = 0; pass < passes; ++pass) {
    points[3] = goal;
    for (int i = 2; i >= 0; --i) {
      const Vec3 direction = Normalize(Sub(points[i], points[i + 1]), pole);
      points[i] = Add(points[i + 1], Scale(direction, lengths[i]));
    }
    points[0] = shoulder;
    for (int i = 0; i < 3; ++i) {
      const Vec3 direction = Normalize(Sub(points[i + 1], points[i]), forward);
      points[i + 1] = Add(points[i], Scale(direction, lengths[i]));
    }
  }

  // A final exact end-effector correction keeps the public contract stable
  // when the caller requests zero iterations or a numerically awkward pole.
  points[3] = goal;
  points[2] = Sub(points[3], Scale(Normalize(Sub(points[3], points[2]), forward),
                                  lengths[2]));
  points[1] = Sub(points[2], Scale(Normalize(Sub(points[2], points[1]), pole),
                                  lengths[1]));
  points[0] = shoulder;

  SetSegmentPose(&result.shoulder, points[0], points[1]);
  SetSegmentPose(&result.elbow, points[1], points[2]);
  SetSegmentPose(&result.wrist, points[2], points[3]);
  result.hand = target.hand;
  result.hand.position = points[3];
  result.hand.valid = true;
  result.reach_error = Length(Sub(points[3], target.hand.position));
  result.clamped = clamped;
  result.valid = true;
  return result;
}

ArmFrame BuildMotionArmFrame(const MotionBody& body, const MotionHand& left,
                             const MotionHand& right) {
  ArmFrame frame;
  const Vec3 origin = body.body_origin;
  ArmRig left_rig;
  left_rig.shoulder = Add(origin, {-0.22, 1.42, 0.0});
  left_rig.pole = Add(left_rig.shoulder, {-0.12, 0.0, -0.35});
  ArmRig right_rig;
  right_rig.shoulder = Add(origin, {0.22, 1.42, 0.0});
  right_rig.pole = Add(right_rig.shoulder, {0.12, 0.0, -0.35});
  frame.left = SolveArm(left_rig, {left.pose, left_rig.pole, 1.0});
  frame.right = SolveArm(right_rig, {right.pose, right_rig.pole, 1.0});
  frame.valid = frame.left.valid || frame.right.valid;
  return frame;
}

void ArmFrameMailbox::publish(const ArmFrame& frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  frame_ = frame;
  ++sequence_;
}

bool ArmFrameMailbox::latest(ArmFrame* out) const {
  if (out == nullptr) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (sequence_ == 0 || !frame_.valid) return false;
  *out = frame_;
  return true;
}

}  // namespace mecvr::ik
