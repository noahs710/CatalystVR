# Square eye composition design

## Goal

Ensure every immersive XR eye receives an aspect-preserving composition with
the runtime-selected eye texture as the destination. Desktop 16:9 backbuffers
remain an input source only; they must never become a letterboxed theatre image
inside the headset.

## Current defect

The shared GPU path already uses `CenterCropUv` when drawing a captured frame
into each OpenXR swapchain image. The CPU fallback instead scales the complete
source into the destination and clears the unused area. With a 16:9 source and
a square Quest eye texture this produces black bars and a theatre-like frame,
even though the XR layer itself is a projection layer.

## Chosen approach

Use one composition rule in both transports:

1. Keep the OpenXR runtime's recommended per-eye width, height, refresh rate,
   and swapchain allocation unchanged.
2. Compute a centered crop from source dimensions to destination dimensions
   with the existing `CenterCropUv` helper.
3. For the CPU path, resample only the crop into every destination pixel. Do
   not clear or preserve an unused letterbox region.
4. Keep the GPU path's shader crop unchanged and document the two paths as the
   same policy.
5. Keep independent capture cadence for the two temporal eyes. A left-eye
   present must not suppress the right-eye present that completes its pair.

This preserves the headset runtime's requested resolution while guaranteeing
that a desktop aspect ratio cannot leak into the immersive projection as black
framing or a stretched image.

## Failure behavior

Invalid dimensions still reject the upload. If staging allocation or mapping
fails, the existing frame worker rejects the frame and ends the XR frame
without partially submitting either eye.

## Verification

- The shared blit math test covers the source-to-target aspect cases used by
  the CPU sampler.
- The M2B transport test records the worker's real per-eye upload calls,
  proves independent pixels reach both projection eyes, proves mono is not
  used, and rejects a stale cross-tick pair.
- The temporal capture path uses per-eye throttles so consecutive game
  presents form a pair reliably at high refresh rates.
- Run the complete Release CTest suite, launcher checks, package self-test,
  and a headless retail smoke. No HMD is required for these checks.
