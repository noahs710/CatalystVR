#include "render/body_overlay_geometry.h"

#include <algorithm>
#include <cmath>

namespace mecvr::render {
namespace {

using Joint = mecvr::ik::BodyJoint;

struct Point2 {
  float x = 0.0f;
  float y = 0.0f;
  float depth = 0.5f;
};

struct Point3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

Point3 Rotate(const mecvr::camera::Quat& q, Point3 value) {
  const Point3 u{q.x, q.y, q.z};
  const Point3 uv{u.y * value.z - u.z * value.y,
                  u.z * value.x - u.x * value.z,
                  u.x * value.y - u.y * value.x};
  const Point3 uuv{u.y * uv.z - u.z * uv.y,
                   u.z * uv.x - u.x * uv.z,
                   u.x * uv.y - u.y * uv.x};
  return {value.x + 2.0 * (q.w * uv.x + uuv.x),
          value.y + 2.0 * (q.w * uv.y + uuv.y),
          value.z + 2.0 * (q.w * uv.z + uuv.z)};
}

Point3 HeadLocal(const mecvr::ik::HumanoidPoseFrame& pose, Point3 point) {
  const auto& head = pose.joint(Joint::kHead);
  const Point3 delta{point.x - head.position.x, point.y - head.position.y,
                     point.z - head.position.z};
  // The overlay is composited after the game scene, so its 2D coordinates
  // must be expressed in the current HMD view rather than world axes. The
  // conjugate is the inverse for a normalized tracking quaternion.
  const mecvr::camera::Quat inverse{-head.orientation.x,
                                    -head.orientation.y,
                                    -head.orientation.z,
                                    head.orientation.w};
  return Rotate(inverse, delta);
}

Point2 Project(const mecvr::ik::HumanoidPoseFrame& pose, Joint joint,
               float eye_offset_x) {
  const auto& position = pose.joint(joint).position;
  Point3 local = HeadLocal(pose, {position.x, position.y, position.z});
  local.x -= static_cast<double>(eye_offset_x);
  const double perspective =
      1.0 / std::clamp(1.0 + local.z * 0.35, 0.78, 1.22);
  return {static_cast<float>(std::clamp(local.x * 1.35 * perspective,
                                        -0.98, 0.98)),
          static_cast<float>(std::clamp(local.y * 1.25 * perspective,
                                        -0.92, 0.92)),
          static_cast<float>(std::clamp(0.50 + local.z * 0.22, 0.05, 0.95))};
}

Point2 ProjectPoint(const mecvr::ik::HumanoidPoseFrame& pose, Point3 point,
                    float eye_offset_x) {
  Point3 local = HeadLocal(pose, point);
  local.x -= static_cast<double>(eye_offset_x);
  const double perspective =
      1.0 / std::clamp(1.0 + local.z * 0.35, 0.78, 1.22);
  return {static_cast<float>(std::clamp(local.x * 1.35 * perspective,
                                        -0.98, 0.98)),
          static_cast<float>(std::clamp(local.y * 1.25 * perspective,
                                        -0.92, 0.92)),
          static_cast<float>(std::clamp(0.50 + local.z * 0.22, 0.05, 0.95))};
}

void Push(BodyOverlayVertex* output, std::size_t capacity, std::size_t* count,
          Point2 p, float r, float g, float b, float a = 0.94f) {
  if (*count >= capacity) return;
  output[(*count)++] = {p.x, p.y, p.depth, r, g, b, a};
}

void Capsule(BodyOverlayVertex* output, std::size_t capacity,
             std::size_t* count, Point2 a, Point2 b, float width, float r,
             float g, float blue, float alpha = 0.94f) {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float length = std::sqrt(dx * dx + dy * dy);
  if (length < 1e-5f) return;
  const Point2 n{-dy / length * width, dx / length * width};
  const Point2 a0{a.x - n.x, a.y - n.y, a.depth};
  const Point2 a1{a.x + n.x, a.y + n.y, a.depth};
  const Point2 b0{b.x - n.x, b.y - n.y, b.depth};
  const Point2 b1{b.x + n.x, b.y + n.y, b.depth};
  Push(output, capacity, count, a0, r, g, blue, alpha);
  Push(output, capacity, count, a1, r, g, blue, alpha);
  Push(output, capacity, count, b1, r, g, blue, alpha);
  Push(output, capacity, count, a0, r, g, blue, alpha);
  Push(output, capacity, count, b1, r, g, blue, alpha);
  Push(output, capacity, count, b0, r, g, blue, alpha);
}

void Disc(BodyOverlayVertex* output, std::size_t capacity,
          std::size_t* count, Point2 center, float radius, float r, float g,
          float b) {
  constexpr int kSegments = 8;
  constexpr float kTau = 6.28318530718f;
  for (int i = 0; i < kSegments; ++i) {
    const float a0 = kTau * static_cast<float>(i) / kSegments;
    const float a1 = kTau * static_cast<float>(i + 1) / kSegments;
    Push(output, capacity, count, center, r, g, b);
    Push(output, capacity, count,
         {center.x + std::cos(a0) * radius,
          center.y + std::sin(a0) * radius, center.depth},
         r, g, b);
    Push(output, capacity, count,
         {center.x + std::cos(a1) * radius,
          center.y + std::sin(a1) * radius, center.depth},
         r, g, b);
  }
}

void HandFingers(BodyOverlayVertex* output, std::size_t capacity,
                 std::size_t* count, Point2 palm, float side, float grip,
                 float trigger, float r, float g, float b) {
  const float closed = std::clamp(grip, 0.0f, 1.0f);
  const float trigger_curl = std::clamp(trigger, 0.0f, 1.0f);
  // Four fingers use two articulated phalanges. The index finger receives
  // extra trigger curl, making the shooting hand visibly react before the
  // game-side mouse action fires.
  for (int finger = 0; finger < 4; ++finger) {
    const float spread = (static_cast<float>(finger) - 1.5f) * 0.022f * side;
    const float finger_curl =
        std::clamp(closed * 0.78f + (finger == 1 ? trigger_curl * 0.22f : 0.0f),
                   0.0f, 1.0f);
    const Point2 base{palm.x + spread, palm.y + 0.024f, palm.depth};
    const Point2 middle{
        palm.x + spread * (1.0f - finger_curl * 0.28f),
        palm.y + 0.065f * (1.0f - finger_curl * 0.42f), palm.depth};
    const Point2 tip{palm.x + spread * (1.0f - finger_curl * 0.62f),
                     palm.y + 0.112f * (1.0f - finger_curl) -
                         0.018f * finger_curl, palm.depth};
    Capsule(output, capacity, count, base, middle, 0.007f, r, g, b, 0.90f);
    Capsule(output, capacity, count, middle, tip, 0.007f, r, g, b, 0.90f);
  }
  const Point2 thumb_base{palm.x + side * 0.035f, palm.y + 0.012f,
                          palm.depth};
  const Point2 thumb_tip{palm.x + side * (0.073f - closed * 0.028f),
                         palm.y + 0.060f - closed * 0.030f, palm.depth};
  Capsule(output, capacity, count, thumb_base, thumb_tip, 0.009f, r, g, b,
          0.92f);
}

void MuzzleFlash(BodyOverlayVertex* output, std::size_t capacity,
                 std::size_t* count, Point2 muzzle, float side) {
  const Point2 tip{muzzle.x, muzzle.y + 0.095f, muzzle.depth};
  const Point2 left{muzzle.x - 0.045f * side, muzzle.y + 0.035f,
                    muzzle.depth};
  const Point2 right{muzzle.x + 0.045f * side, muzzle.y + 0.035f,
                     muzzle.depth};
  const Point2 bottom{muzzle.x, muzzle.y - 0.025f, muzzle.depth};
  Push(output, capacity, count, muzzle, 1.0f, 0.88f, 0.22f);
  Push(output, capacity, count, left, 1.0f, 0.30f, 0.04f);
  Push(output, capacity, count, tip, 1.0f, 0.88f, 0.22f);
  Push(output, capacity, count, muzzle, 1.0f, 0.88f, 0.22f);
  Push(output, capacity, count, tip, 1.0f, 0.88f, 0.22f);
  Push(output, capacity, count, right, 1.0f, 0.30f, 0.04f);
  Push(output, capacity, count, muzzle, 1.0f, 0.88f, 0.22f);
  Push(output, capacity, count, right, 1.0f, 0.30f, 0.04f);
  Push(output, capacity, count, bottom, 1.0f, 0.88f, 0.22f);
}

}  // namespace

std::size_t BuildBodyOverlayGeometry(
    const mecvr::ik::HumanoidPoseFrame& pose, BodyOverlayVertex* output,
    std::size_t capacity, float eye_offset_x) {
  if (!pose.valid || output == nullptr || capacity == 0) return 0;
  std::size_t count = 0;
  const auto add = [&](Joint a, Joint b, float width, float r, float g,
                       float blue) {
    Capsule(output, capacity, &count, Project(pose, a, eye_offset_x),
            Project(pose, b, eye_offset_x),
            width, r, g, blue);
  };
  add(Joint::kPelvis, Joint::kChest, 0.045f, 0.82f, 0.82f, 0.88f);
  add(Joint::kChest, Joint::kNeck, 0.035f, 0.82f, 0.82f, 0.88f);
  add(Joint::kLeftShoulder, Joint::kLeftElbow, 0.032f, 0.08f, 0.74f, 1.0f);
  add(Joint::kLeftElbow, Joint::kLeftWrist, 0.027f, 0.08f, 0.74f, 1.0f);
  add(Joint::kLeftWrist, Joint::kLeftHand, 0.022f, 0.08f, 0.74f, 1.0f);
  add(Joint::kRightShoulder, Joint::kRightElbow, 0.032f, 1.0f, 0.28f, 0.12f);
  add(Joint::kRightElbow, Joint::kRightWrist, 0.027f, 1.0f, 0.28f, 0.12f);
  add(Joint::kRightWrist, Joint::kRightHand, 0.022f, 1.0f, 0.28f, 0.12f);
  add(Joint::kLeftHip, Joint::kLeftKnee, 0.040f, 0.68f, 0.68f, 0.76f);
  add(Joint::kLeftKnee, Joint::kLeftAnkle, 0.032f, 0.68f, 0.68f, 0.76f);
  add(Joint::kRightHip, Joint::kRightKnee, 0.040f, 0.68f, 0.68f, 0.76f);
  add(Joint::kRightKnee, Joint::kRightAnkle, 0.032f, 0.68f, 0.68f, 0.76f);

  const Point2 left_hand = Project(pose, Joint::kLeftHand, eye_offset_x);
  const Point2 right_hand = Project(pose, Joint::kRightHand, eye_offset_x);
  Disc(output, capacity, &count, left_hand, 0.06f, 0.10f, 0.82f, 1.0f);
  Disc(output, capacity, &count, right_hand, 0.06f, 1.0f, 0.34f, 0.12f);
  HandFingers(output, capacity, &count, left_hand, -1.0f,
              pose.grip_values[0], pose.trigger_values[0], 0.16f, 0.88f,
              1.0f);
  HandFingers(output, capacity, &count, right_hand, 1.0f,
              pose.grip_values[1], pose.trigger_values[1], 1.0f, 0.40f,
              0.16f);
  // Trigger pressure turns each hand marker into a weapon silhouette. The
  // muzzle follows the tracked hand orientation, while grip pressure changes
  // the silhouette thickness to make two-hand/closed-grip intent visible.
  const auto weapon = [&](Joint joint, std::size_t hand, Point2 hand_point) {
    if (pose.trigger_values[hand] <= 0.15f) return;
    const auto& tracked = pose.joint(joint);
    const Point3 direction = Rotate(tracked.orientation, {0.0, 0.0, -1.0});
    const auto& position = tracked.position;
    const Point2 muzzle = ProjectPoint(
        pose, {position.x + direction.x * 0.24, position.y + direction.y * 0.24,
               position.z + direction.z * 0.24}, eye_offset_x);
    const float grip = std::clamp(pose.grip_values[hand], 0.0f, 1.0f);
    Capsule(output, capacity, &count, hand_point, muzzle,
            0.018f + grip * 0.018f, 0.70f, 0.76f, 0.80f);
    Capsule(output, capacity, &count, hand_point,
            {hand_point.x, hand_point.y + 0.08f, hand_point.depth},
            0.014f + grip * 0.01f,
            0.34f, 0.36f, 0.40f);
    if (pose.shot_pulses[hand]) MuzzleFlash(output, capacity, &count, muzzle,
                                             hand == 0 ? -1.0f : 1.0f);
  };
  weapon(Joint::kLeftHand, 0, left_hand);
  weapon(Joint::kRightHand, 1, right_hand);
  return count;
}

}  // namespace mecvr::render
