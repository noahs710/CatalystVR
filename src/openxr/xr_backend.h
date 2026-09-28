#pragma once

// XR backend interface (plan T5 deliverable 1, owned by the render stream).
//
// IXrBackend is the seam between MECVR's XR frame worker and any session
// implementation. MockXRBackend implements it now (M1A); RealOpenXRBackend
// implements the same interface later (M1B, plan T8) against the Khronos
// loader with XR_KHR_D3D11_enable. Real-API mapping per method:
//
//   startup/shutdown  -> xrCreateInstance, xrGetSystem, xrCreateSession,
//                        xrDestroyInstance (+ D3D11 requirements query).
//   waitFrame         -> xrWaitFrame (authoritative scheduling; predicted
//                        display time + interval flow through the pipeline).
//   beginFrame        -> xrBeginFrame.
//   locateViews       -> xrLocateViews (space selects the base reference
//                        space; both eyes always located together).
//   acquire/release   -> xrAcquireSwapchainImage / xrReleaseSwapchainImage.
//   endFrame          -> xrEndFrame (submitted=false maps to
//                        XR_ENVIRONMENT_BLEND_MODE / no-layer submit path).
//   recenter         -> LOCAL-space re-baseline (request path for the real
//                        backend; applied immediately in the mock).
//   controllerState   -> xrSyncActions + xrGetActionStateBoolean/Pose for
//                        grip action spaces created via xrCreateActionSpace.
//   displayFrequency  -> XR_FB_display_refresh_rate where exposed, else the
//                        xrWaitFrame-derived cadence (Key Decision 4).
//   viewConfig        -> xrEnumerateViewConfigurationViews (recommended +
//                        maximum swapchain extents per eye).

#include <array>
#include <cstdint>

#include "openxr/xr_types.h"
#include "render/shared_capture_mailbox.h"

namespace mecvr::openxr {

// Reference spaces. Maps to XrReferenceSpaceType entries
// XR_REFERENCE_SPACE_TYPE_VIEW / LOCAL / STAGE (Key Decision 9:
// LOCAL primary, STAGE where available, VIEW head-locked fallback).
enum class Space { kView, kLocal, kStage };

// Maps to the left/right subaction paths of controller actions.
enum class Hand { kLeft, kRight };

// Synthetic frame schedule. Maps to XrFrameState: predictedDisplayTime,
// predictedDisplayPeriod, shouldRender, plus the frame counter the mock
// derives times from.
struct FrameTiming {
  XrTime predicted_display_time_ns = 0;
  XrTime predicted_display_period_ns = 0;
  std::uint64_t frame_index = 0;
  bool should_render = true;
};

// Both-eye locate result. Element 0 is left, element 1 is right.
struct LocatedViews {
  std::array<XrView, 2> views;
  XrTime sample_time_ns = 0;
  Space space = Space::kLocal;
};

inline constexpr std::size_t kBodyTrackingJointCount = 21;

// Backend-owned canonical body snapshot. It intentionally contains only seam
// types; OpenXR tracker handles and extension structs stay private to the real
// backend implementation.
struct BodyTrackingSnapshot {
  std::array<XrPosef, kBodyTrackingJointCount> joints{};
  std::uint32_t valid_mask = 0;
  XrTime sample_time_ns = 0;
  float confidence = 0.0f;
  bool active = false;
};

// Per-eye swapchain sizing. Maps to XrViewConfigurationView
// (recommendedWidth/Height, maxWidth/Height).
struct ViewConfig {
  std::uint32_t recommended_width = 0;
  std::uint32_t recommended_height = 0;
  std::uint32_t max_width = 0;
  std::uint32_t max_height = 0;
  std::uint32_t swapchain_width = 0;
  std::uint32_t swapchain_height = 0;
};

struct SwapchainSize {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

inline SwapchainSize SelectSwapchainSize(const ViewConfig& config,
                                         std::uint32_t requested_square) {
  std::uint32_t target = requested_square != 0 ? requested_square
                                               : config.recommended_width;
  if (target == 0) target = 1;
  std::uint32_t width = target;
  std::uint32_t height = target;
  if (config.max_width != 0 && width > config.max_width)
    width = config.max_width;
  if (config.max_height != 0 && height > config.max_height)
    height = config.max_height;
  if (config.recommended_width != 0 && width < config.recommended_width)
    width = config.recommended_width;
  if (config.recommended_height != 0 && height < config.recommended_height)
    height = config.recommended_height;
  if (config.max_width != 0 && width > config.max_width)
    width = config.max_width;
  if (config.max_height != 0 && height > config.max_height)
    height = config.max_height;
  return {width, height};
}

// Controller snapshot: grip-pose location plus boolean action states
// collapsed to a bitmask. pose_valid carries the XrSpaceLocation pose
// validity bits; buttons carry the per-button action states.
struct ControllerState {
  XrPosef grip_pose;
  bool pose_valid = false;
  std::uint32_t buttons = 0u;
  float thumbstick_x = 0.0f;
  float thumbstick_y = 0.0f;
  // Analog values are retained alongside the digital action bits so motion
  // hands can drive authored finger/hand poses without re-sampling OpenXR.
  float trigger_value = 0.0f;
  float squeeze_value = 0.0f;
};

inline constexpr std::uint32_t kButtonTrigger = 1u << 0;
inline constexpr std::uint32_t kButtonSqueeze = 1u << 1;
inline constexpr std::uint32_t kButtonThumbstick = 1u << 2;
inline constexpr std::uint32_t kButtonPrimary = 1u << 3;    // A / X
inline constexpr std::uint32_t kButtonSecondary = 1u << 4;  // B / Y
inline constexpr std::uint32_t kButtonMenu = 1u << 5;

class IXrBackend {
 public:
  virtual ~IXrBackend() = default;
  IXrBackend(const IXrBackend&) = delete;
  IXrBackend& operator=(const IXrBackend&) = delete;

  virtual bool startup() = 0;
  virtual void shutdown() = 0;
  virtual bool running() const = 0;

  // Frame loop. Called once per frame on the XR worker; the Present hook
  // must never call waitFrame (Key Decision 3).
  virtual FrameTiming waitFrame() = 0;
  virtual bool beginFrame() = 0;
  virtual LocatedViews locateViews(Space space) = 0;
  virtual std::uint32_t acquireSwapchainImage(std::uint32_t view_index) = 0;
  virtual void releaseSwapchainImage(std::uint32_t view_index) = 0;
  // Uploads one eye's RGBA pixels into the CURRENTLY ACQUIRED swapchain
  // image (acquire first). Called on the XR worker between acquire and
  // release. Default no-op (mock/test backends); the real backend copies
  // into the D3D11 swapchain texture. Returns false on failure (frame is
  // skipped, never partially submitted).
  virtual bool uploadEyeImage(std::uint32_t view_index, const std::uint8_t* rgba,
                              std::uint32_t width, std::uint32_t height) {
    (void)view_index;
    (void)rgba;
    (void)width;
    (void)height;
    return true;
  }
  // Ensures the mono composition target exists at the given source dims
  // BEFORE the tick's acquire calls (M2B quad sizing). Default no-op true
  // (mock/test backends + projection path); the real backend creates (or
  // rebuilds after a game resize) its quad chain here so acquire/upload
  // never straddle a chain swap. Always returns true: quad failure falls
  // back to the projection path, never fails the frame.
  virtual bool ensureMonoLayer(std::uint32_t width, std::uint32_t height) {
    (void)width;
    (void)height;
    return true;
  }
  // Switches the real backend to two independent projection swapchains.
  // Default no-op keeps mock/test backends and the mono transport unchanged.
  virtual bool enableStereoProjection() { return true; }
  // Optional GPU-only transport. Registration opens immutable shared handles
  // on the runtime's D3D device; submit draws the selected shared slot into
  // both currently acquired eye images. Backends without this capability
  // return false and retain the CPU upload path.
  virtual bool registerSharedCapture(
      const render::SharedCaptureRegistration& registration) {
    (void)registration;
    return false;
  }
  virtual bool submitSharedFrame(const render::SharedCaptureFrame& frame) {
    (void)frame;
    return false;
  }
  virtual void unregisterSharedCapture(std::uint64_t generation) {
    (void)generation;
  }
  virtual bool endFrame(bool submitted) = 0;

  // Spaces.
  virtual void recenter() = 0;

  // Input.
  virtual ControllerState controllerState(Hand hand) = 0;

  // Timing + views.
  virtual float displayFrequencyHz() const = 0;
  virtual ViewConfig viewConfig(std::uint32_t view_index) const = 0;
  virtual std::uint32_t viewCount() const = 0;

 protected:
  IXrBackend() = default;
};

}  // namespace mecvr::openxr
