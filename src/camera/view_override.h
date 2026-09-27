#pragma once

#include <cstddef>

#include "camera/conventions.h"
#include "camera/math.h"

namespace mecvr::camera {

// Applies a relative HMD rotation to a game-produced view matrix while
// preserving the camera position. The input delta is anchor^-1 * current.
// No projection or translation is changed independently.
Mat4 ApplyViewRotationDelta(const Mat4& base_view, const Quat& delta,
                            MultOrder order);

// Applies the same anchor-relative rotation plus a local HMD translation.
// Translation is scaled in game units per XR meter and clamped by the caller
// before this function is used.
Mat4 ApplyViewPoseDelta(const Mat4& base_view, const Quat& delta,
                        Vec3 local_translation_m, double units_per_meter,
                        MultOrder order);

// Guarded CB rewrite used by the retail M3b bridge. `floats` points at the
// mapped constant-buffer contents; the view is at the supplied float offset.
// Returns false unless the target looks like an orthonormal view matrix.
bool RewriteViewMatrix(float* floats, std::size_t float_count,
                       std::size_t view_offset_floats, const Quat& delta,
                       MultOrder order);

bool RewriteViewPoseMatrix(float* floats, std::size_t float_count,
                           std::size_t view_offset_floats, const Quat& delta,
                           Vec3 local_translation_m, double units_per_meter,
                           MultOrder order);

}  // namespace mecvr::camera
