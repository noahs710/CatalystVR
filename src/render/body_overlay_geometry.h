#pragma once

#include <cstddef>

#include "camera/conventions.h"
#include "ik/full_body_ik.h"

namespace mecvr::render {

struct BodyOverlayVertex {
  float x = 0.0f;
  float y = 0.0f;
  // View-space depth cue: 0 is nearest and 1 is farther from the HMD. The
  // renderer uses it for perspective-preserving shading without depending on
  // the game's private depth buffer.
  float depth = 0.5f;
  float r = 1.0f;
  float g = 1.0f;
  float b = 1.0f;
  float a = 1.0f;
};

struct BodyOverlayView {
  mecvr::camera::Vec3 position{};
  mecvr::camera::Quat orientation{};
  mecvr::camera::FrustumTangents frustum{};
  bool valid = false;
};

// Builds a first-person, screen-facing mod-owned body mesh from the canonical
// IK frame. The output is a triangle list and has no graphics API dependency.
std::size_t BuildBodyOverlayGeometry(
    const mecvr::ik::HumanoidPoseFrame& pose, BodyOverlayVertex* output,
    std::size_t capacity, const BodyOverlayView* view = nullptr);

}  // namespace mecvr::render
