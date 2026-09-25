# MockXR Backend (M1A, plan T5)

Deterministic OpenXR substitute that drives the M1C test scene (plan T9)
with no headset, no OpenXR SDK linkage, and no game interaction. Owned by
the render stream; lives entirely in `src/openxr/`.

## Files

- `src/openxr/xr_types.h` — minimal OpenXR-mapped math types (`XrPosef`,
  `XrQuaternionf`, `XrVector3f`, `XrFovf`, `XrView`, `XrTime`) plus pose
  helpers (`IdentityPose`, `YawQuaternion`, `RelativePose`, `ComposePose`).
- `src/openxr/xr_backend.h` — `IXrBackend` interface: frame loop
  (`waitFrame` / `beginFrame` / `locateViews` / `acquireSwapchainImage` /
  `releaseSwapchainImage` / `endFrame`), spaces (`Space::{View,Local,Stage}`
  + `recenter`), input (`controllerState`, `kButton*` mask), timing
  (`FrameTiming`, `displayFrequencyHz`), views (`LocatedViews`,
  `ViewConfig`, `viewCount`). Each member notes its real-API counterpart
  for M1B.
- `src/openxr/mock_xr_backend.h` / `.cc` — `MockXRBackend` implementing
  `IXrBackend`: configurable HMD pose, synthetic controller poses, fake
  display frequency, recommended/max resolutions, button injection,
  deterministic synthetic timing, head-motion trajectories, and
  record/replay.
- `src/openxr/mock_xr_test.cc` — deterministic validation exe (see below).

## Behavior

- Timing: `predicted_display_time = start + frame_index * period`, period
  derived from the configured frequency (90 Hz → 11111111 ns). Strictly
  monotonic, constant interval.
- Trajectories (`TrajectoryMode`): `kStatic`, `kSineYaw` (amplitude +
  frequency configurable), `kCircle` (radius + frequency), `kReplay`.
  All are pure functions of frame time — deterministic by construction.
- Record/replay: `beginRecording` captures each `waitFrame`'s (timestamp,
  raw head pose); `stopRecording` returns the array. `startReplay` feeds
  the array back verbatim (bit-exact float copies of time and pose); on
  exhaustion the last pose holds while time keeps period-stepping.
- Spaces: LOCAL is recenter-corrected; `recenter()` snapshots the current
  raw head pose as the new LOCAL baseline. VIEW is head-locked (identity
  head + eye offsets); STAGE composes a configurable room origin and
  ignores recenter. Eye poses are the head pose plus a rotated ±ipd/2
  x-offset with per-eye asymmetric FOVs.
- Input: `injectControllerState` / `setControllerPose` /
  `setControllerButtons` round-trip through `controllerState`, including
  full-mask (0xFFFFFFFF) buttons and invalid-pose flags.

## Real-API mapping for M1B (plan T8)

`RealOpenXRBackend` implements the same `IXrBackend` against the Khronos
loader with `XR_KHR_D3D11_enable`: `startup` → instance/system/session
creation (+ `xrGetD3D11GraphicsRequirementsKHR` adapter/LUID gate),
`waitFrame` → `xrWaitFrame` on the XR worker, `locateViews` →
`xrLocateViews`, swapchain methods → acquire/release image, `endFrame` →
`xrEndFrame`, `recenter` → LOCAL re-baseline request path,
`controllerState` → `xrSyncActions` + boolean/pose action states from
`xrCreateActionSpace` grip spaces, frequency → `XR_FB_display_refresh_rate`
where exposed else the `xrWaitFrame`-derived cadence, `viewConfig` →
`xrEnumerateViewConfigurationViews`.

## Validation

Compiled directly with MSVC, zero warnings as errors:

```bat
VsDevCmd.bat -arch=x64
cl.exe /nologo /W4 /WX /EHsc /std:c++17 /I src ^
  src\openxr\mock_xr_backend.cc src\openxr\mock_xr_test.cc
```

Test output (exit 0):

```text
[PASS] replay: recording holds one pose per frame
[PASS] replay: bit-exact across backends and runs
[PASS] replay: poses reproduce recorded trajectory
[PASS] timing: predicted display time advances monotonically
[PASS] timing: frame interval is a constant period
[PASS] timing: 90 Hz period is 11111111 ns
[PASS] recenter: LOCAL reflects head pose before recenter
[PASS] recenter: re-baselines LOCAL to identity head pose
[PASS] recenter: VIEW space unaffected
[PASS] input: left controller pose/button injection round-trips
[PASS] input: right controller pose/button injection round-trips
[PASS] input: pose/button setters compose with injection
[PASS] views: two eyes reported
[PASS] views: recommended resolution nonzero and within max
[PASS] views: per-eye FOV is asymmetric
[PASS] timing: fake display frequency is configurable
[PASS] timing: 72 Hz period is 13888889 ns
[PASS] frame: begin/end sequencing is guarded
[PASS] views: out-of-range view index yields empty config
[PASS] frame: out-of-range swapchain view yields image 0
ALL TESTS PASSED
```

## Boundaries

Mock only: no session, no swapchains (image indices cycle 0–2), no game
writes, no camera/stereo/gameplay work (STOP S3 respected). STAGE origin
defaults to identity; real STAGE bounds come in M1B.
