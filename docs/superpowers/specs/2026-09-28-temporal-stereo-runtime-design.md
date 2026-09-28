# Temporal Stereo Runtime Repair Design

**Date:** 2026-09-28

## Evidence and goal

Retail HMD logs show the runtime reaching `VirtualDesktopXR` with two valid
views, but the public temporal path reports `stereo_submit=0` and a rapidly
growing `stereo_reject` count. The same camera bridge produces successful
overrides when stereo is disabled. The current producer advances its eye bit on
every game Present even when the per-eye capture throttle rejected that
Present, so the camera pose selected for a captured image is not guaranteed to
match the pair state. The current XR worker also compares the producer's XR
epoch to the worker's immediately current frame index; game Present cadence and
XR cadence are independent, so a valid fresh pair can be rejected solely for
arriving more than one XR tick later.

This slice makes temporal stereo a functioning, bounded presentation path. It
does not claim simultaneous engine-native dual-pass rendering; that remains the
future quality/performance path.

## Design

### Producer state machine

`LiveCapture::captureStereo` returns whether the requested eye was actually
captured. `HookPresent` advances from left to right, or right to left, only
after a successful capture. A throttled Present therefore leaves the current
eye selected instead of silently changing the camera pose for the next
capture. The pair continues to require one unchanged epoch and pose sequence,
equal square dimensions, complete payloads, and a bounded inter-eye interval.

### Consumer acceptance

The XR worker keeps the pair's immutable epoch/pose identity for diagnostics,
but does not compare it to the current XR frame index. Pair identity is already
proven at the producer boundary. The consumer accepts a frame only when it is
structurally valid and its capture timestamp is within a bounded presentation
freshness window. Invalid, stale, and backend/upload failures receive separate
counters so a future HMD run distinguishes synchronization from transport
failure.

### Camera and fallback behavior

The existing camera override remains fail-closed. A rejected stereo pair falls
back to the newest mono frame for that XR tick, never to a theatre quad when
immersive stereo is requested. The OpenXR backend continues to submit the
independent eye images through a projection layer with the runtime-located
per-eye poses and FOVs. No desktop 16:9 surface is passed as an eye image;
the producer still square-crops at capture time and the backend scales only
into runtime-owned eye swapchains.

## Testing

- Add pure timestamp/freshness tests for valid, stale, future-skewed, and
  malformed stereo frames.
- Extend the temporal capture seam test to prove a skipped capture does not
  advance the eye state and that a later left/right pair is published with one
  pose identity.
- Extend XR worker coverage so a valid pair whose producer epoch is several XR
  ticks old but whose capture timestamp is fresh submits successfully, while
  stale and malformed pairs fall back safely.
- Rebuild Release, run the complete CTest suite, package a grouped alpha
  release, and inspect the retail smoke log for the new rejection counters.

## Non-goals

- Guessing a native Faith skeleton palette or enabling unverified writes.
- Replacing the engine's temporal producer with a simultaneous native dual-pass
  renderer in this slice.
- Overriding VDXR/OpenXR requested resolution or refresh rate.
