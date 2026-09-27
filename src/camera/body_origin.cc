// P2.0 VR Body Origin implementation. See body_origin.h.

#include "camera/body_origin.h"

namespace mecvr::camera {

Quat QuatBasisChange(Quat q_anchor_local, const Mat4& local_axis_map) {
  const Quat l = MatToQuat(local_axis_map);
  const Quat li = QuatConjugate(l);
  return QuatNormalize(QuatMul(QuatMul(l, QuatNormalize(q_anchor_local)), li));
}

Vec3 AnchorTranslationDeltaGame(const BodyAnchor& anchor,
                                Quat live_game_orientation,
                                Vec3 hmd_current,
                                const WorldConvention& world) {
  // Raw XR-meter delta, anchor-localized: rotate the LOCAL-frame
  // difference back by the anchor orientation so "lean left" keeps
  // its meaning no matter where the head was pointing at capture.
  const Vec3 diff_m = VecSub(hmd_current, anchor.hmd_anchor_position);
  const Quat inv =
      QuatConjugate(QuatNormalize(anchor.hmd_anchor_orientation));
  Vec3 anchor_local_m = QuatRotate(inv, diff_m);
  if (anchor.has_axis_map) {
    // Anchor-local axes -> game-camera-local axes (M3a convention).
    const Mat4& b = anchor.local_axis_map;
    Vec3 r;
    r.x = b.m[0] * anchor_local_m.x + b.m[1] * anchor_local_m.y +
          b.m[2] * anchor_local_m.z;
    r.y = b.m[4] * anchor_local_m.x + b.m[5] * anchor_local_m.y +
          b.m[6] * anchor_local_m.z;
    r.z = b.m[8] * anchor_local_m.x + b.m[9] * anchor_local_m.y +
          b.m[10] * anchor_local_m.z;
    anchor_local_m = r;
  }
  // Meters -> game units, then into game-WORLD axes through the LIVE
  // game base orientation (Faith may have turned since capture).
  const Vec3 game_local = XrOffsetToGame(anchor_local_m, world);
  return QuatRotate(QuatNormalize(live_game_orientation), game_local);
}

bool EvaluateVrCamera(const BodyAnchor& anchor, Quat hmd_orientation,
                      Vec3 hmd_position, Quat live_game_orientation,
                      Vec3 live_game_position,
                      std::uint64_t space_generation,
                      const WorldConvention& world, VrCameraPose* out) {
  if (out == nullptr || !anchor.valid) return false;
  if (space_generation != anchor.space_generation) return false;
  const Quat live = QuatNormalize(live_game_orientation);
  const Quat delta = AnchorRotationDelta(anchor, hmd_orientation);
  out->orientation = QuatNormalize(QuatMul(live, delta));
  const Vec3 t = AnchorTranslationDeltaGame(anchor, live, hmd_position,
                                            world);
  out->position = VecAdd(live_game_position, t);
  return true;
}

}  // namespace mecvr::camera
