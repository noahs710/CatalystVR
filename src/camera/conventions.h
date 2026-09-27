#pragma once

// P2.0 projection/world conventions (plan Decision 6). Catalyst's real
// convention is UNKNOWN until M3a observes it, so every axis below is
// an explicit parameter and the test suite covers the full matrix.
// Nothing here defaults the game into an assumed convention: callers
// pass the observed (or hypothesized-then-falsified) convention in.

#include "camera/math.h"

namespace mecvr::camera {

// Which side vectors multiply on. Row-vector (D3D style): p' = p*M.
// Column-vector (GL style): p' = M*p.
enum class MultOrder { kRowVector, kColumnVector };

// Depth clip range after perspective divide.
enum class ClipZ { kZeroToOne, kNegOneToOne };

// Right-handed (OpenXR/D3D view space: camera looks along -Z with
// +X right, +Y up) vs left-handed (camera looks along +Z).
enum class Handedness { kRight, kLeft };

struct ProjectionConvention {
  MultOrder mult = MultOrder::kColumnVector;
  ClipZ clip_z = ClipZ::kZeroToOne;
  Handedness handed = Handedness::kRight;
  bool reverse_z = false;   // Depth 1 at near, 0 at far.
  bool infinite_far = false;  // Far plane at infinity (drops far term).
};

// Asymmetric frustum as tangent-of-angle extents. Sign convention
// matches XrFovf: left/down are NEGATIVE tangents, right/up positive.
struct FrustumTangents {
  double left = -1.0;
  double right = 1.0;
  double up = 1.0;
  double down = -1.0;
};

// Builds the projection matrix for the given frustum + near/far under
// the given convention. Returns Mat4 in row-major storage; interpret
// with MatTransformPoint(..., row_vector = (mult == kRowVector)).
// near/far must be positive distances; far is ignored when
// infinite_far is set. Returns identity on degenerate input
// (caller must validate; tested).
Mat4 BuildProjection(const FrustumTangents& frustum, double near_dist,
                     double far_dist, const ProjectionConvention& conv);

// Packs a Mat4 into 16 floats for constant-buffer upload in the
// requested storage order (independent of mult order: D3D constant
// buffers may hold either; M3a observes which).
// storage_row_major=true: out[i] = m.m[i]; false: transposed.
void PackMatrix(const Mat4& m, bool storage_row_major, float* out16);

// World-frame convention: maps OpenXR meters (+X right, +Y up, -Z
// forward) into game world units. units_per_meter is the M5
// calibration (default 1.0 = uncalibrated); axis_map columns express
// game axes in XR axes (identity = same frame, the common case).
struct WorldConvention {
  double units_per_meter = 1.0;
  // 3x3 row-major: game_vec = axis_map * xr_vec.
  double axis_map[9] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
};

// Converts an XR-space offset (meters) into game-world units.
Vec3 XrOffsetToGame(Vec3 xr_meters, const WorldConvention& conv);

}  // namespace mecvr::camera
