#pragma once

// P2.0 VR Body Origin / recenter math (plan Decision 5).
//
// Absolute LOCAL-space HMD pose is NEVER applied to the game camera.
// At VR activation/recenter the caller captures an anchor (HMD anchor
// orientation + position, game base reference, axis map); per frame
// the HMD DELTA from anchor composes with the LIVE game base camera:
//   VR camera = live game base x HMD delta from recenter
// The game base is live (not captured) so Faith's own turns and moves
// still drive the view; the HMD only adds relative look/lean. At the
// capture instant the delta is identity, so VR forward IS Faith's
// current forward — never the runtime's startup forward.
//
// Translation: (current HMD center - anchor HMD center) in meters,
// anchor-localized, mapped through the axis map + live game base
// orientation into game-world axes, scaled to game units, added to
// the RENDER viewpoint only. Head motion never alters Faith's
// world/player transform (enforced by API: this module outputs a
// render-camera pose, never a player transform).

#include "camera/conventions.h"
#include "camera/math.h"

namespace mecvr::camera {

// Captured once per VR activation / recenter event.
struct BodyAnchor {
  // HMD anchor orientation + position at capture (XR LOCAL frame).
  Quat hmd_anchor_orientation;
  Vec3 hmd_anchor_position;
  // Game base orientation at capture: reference/telemetry (evaluation
  // uses the LIVE base; this records what "Faith's forward" was when
  // the user recentered).
  Quat game_base_at_capture;
  // Anchor-local axes -> game-camera-local axes (rotation part of a
  // Mat4; identity unless M3a convention discovery says otherwise).
  Mat4 local_axis_map;
  bool has_axis_map = false;
  // Matches XRFramePoseSnapshot::space_generation at capture; a
  // generation change invalidates the anchor (caller re-captures).
  std::uint64_t space_generation = 0;
  bool valid = false;
};

inline BodyAnchor CaptureAnchor(Quat hmd_anchor_orientation,
                                Vec3 hmd_anchor_position,
                                Quat game_base_at_capture,
                                std::uint64_t space_generation) {
  BodyAnchor a;
  a.hmd_anchor_orientation = QuatNormalize(hmd_anchor_orientation);
  a.hmd_anchor_position = hmd_anchor_position;
  a.game_base_at_capture = QuatNormalize(game_base_at_capture);
  a.local_axis_map = MatIdentity();
  a.has_axis_map = false;
  a.space_generation = space_generation;
  a.valid = true;
  return a;
}

// Conjugates a local-frame delta quaternion through a basis rotation:
// q_game = L * q_anchor * L^-1. Identity when the frames coincide.
Quat QuatBasisChange(Quat q_anchor_local, const Mat4& local_axis_map);

// HMD rotation delta from anchor, expressed in GAME-camera-local axes:
//   delta = L * (anchor^-1 * current) * L^-1
inline Quat AnchorRotationDelta(const BodyAnchor& anchor, Quat hmd_current) {
  const Quat inv =
      QuatConjugate(QuatNormalize(anchor.hmd_anchor_orientation));
  Quat delta = QuatNormalize(QuatMul(inv, QuatNormalize(hmd_current)));
  if (anchor.has_axis_map) delta = QuatBasisChange(delta, anchor.local_axis_map);
  return QuatNormalize(delta);
}

// HMD translation delta from anchor, in GAME-WORLD units:
//   t = live_game_ori * L * anchor_local(meters->units)
// (caller adds it to the live game base position).
Vec3 AnchorTranslationDeltaGame(const BodyAnchor& anchor,
                                Quat live_game_orientation,
                                Vec3 hmd_current,
                                const WorldConvention& world);

// Full per-frame evaluation. Returns false (out untouched) when the
// anchor is invalid or the space generation moved (caller must
// re-capture and fail gracefully meanwhile).
struct VrCameraPose {
  Quat orientation;  // Game-frame VR camera orientation.
  Vec3 position;     // Game-frame VR camera position (render only).
};
bool EvaluateVrCamera(const BodyAnchor& anchor, Quat hmd_orientation,
                      Vec3 hmd_position, Quat live_game_orientation,
                      Vec3 live_game_position,
                      std::uint64_t space_generation,
                      const WorldConvention& world, VrCameraPose* out);

}  // namespace mecvr::camera
