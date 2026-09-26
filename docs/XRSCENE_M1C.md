# OpenXR Test Scene (M1C, plan T9)

Standalone stereo test scene driving `IXrBackend` on both `MockXRBackend`
(full pass) and `RealOpenXRBackend` (degraded pass — headset absent).
No game, no game thread, no camera or gameplay work (STOP S3 respected:
the scene renders test content into its own offscreen D3D11 targets, never
into a game swapchain; stereo here is standalone test content per the T9
validation plan, not a game stereo hook).

## Files (all new; no existing file touched)

- `src/openxr/test/xr_scene.h` / `.cc` — `SceneRenderer` (own D3D11 device,
  per-eye render target + staging pair, eye-distinguishing tints, readback)
  and `SceneSession` (scripted phase driver; whole frame loop on a dedicated
  XR worker thread).
- `tests/xr_scene/xr_scene_test.cc` — test main (`--backend mock|real|all`,
  default `all`).
- `docs/XRSCENE_M1C.md` — this file.

## Design (Key Decisions 1–4/9)

- Frame-loop ownership (Dec. 3): `xrWaitFrame` + `beginFrame`/`locateViews`/
  acquire/release/`endFrame` + event processing run ONLY on the `SceneSession`
  worker in both paths. The main thread coordinates (startup/shutdown,
  recenter requests via atomics). Thread IDs are asserted different.
- Scene rendering: per-eye offscreen targets sized from `viewConfig()`
  (mock 2064x2104). Left clears reddish (140,18,26), right bluish
  (20,31,158); a centered quad (middle quarter) is drawn per eye
  (orange left, cyan right) with brightness `0.8+0.2*cos(yaw)` coupled to
  the head yaw flowing in from `locateViews(LOCAL)`. Corner + center pixels
  are read back through staging textures and compared to `ExpectedColors`
  (±2 LSB). The D3D11 immediate context is touched on the worker only.
- Session states: harness enum mirroring `XrSessionState` vocabulary. Mock
  script: `IDLE→READY→SYNCHRONIZED→VISIBLE→FOCUSED→VISIBLE(focus
  lost)→FOCUSED(+recenter)→FOCUSED(headset absent)→FOCUSED(recovered)→
  STOPPING`. Focus loss pumps with `endFrame(false)`; absence suspends ALL
  backend calls (10 frames, backend unreachable); recovery resumes with
  monotonic time asserted across the gap.
- Recenter: applied on the worker (phase flag on mock, atomic request on
  real), re-baselining LOCAL; math asserted separately (displaced head →
  identity ±ipd/2).
- Determinism: record-120/replay-on-two-backends compared bit-exact
  (times + poses + orientation), asserted in the test.

## Validation

Compiled directly with MSVC, zero warnings (`/W4 /WX` clean):

```bat
VsDevCmd.bat -arch=x64
cl.exe /nologo /W4 /WX /EHsc /std:c++17 /I src /I third_party\openxr\include ^
  src\openxr\test\xr_scene.cc src\openxr\mock_xr_backend.cc ^
  src\openxr\real_xr_backend.cc tests\xr_scene\xr_scene_test.cc ^
  /link third_party\openxr\lib\openxr_loader.lib d3d11.lib dxgi.lib user32.lib ^
  /OUT:%TEMP%\mecvr-t9\xr_scene_test.exe
```

Run with `third_party\openxr\bin` (loader DLL) staged beside the exe.

Mock full pass (exit 0, this machine):

```text
[PASS] mock: yaw extraction inverts YawQuaternion
[PASS] mock: recording holds one pose per frame
[PASS] mock: deterministic trajectories bit-reproducible
[PASS] mock: replay reproduces recorded trajectory
[PASS] mock: scene renderer starts at recommended extents (2064x2104)
[PASS] mock: backend startup
[PASS] mock: stereo view count
[PASS] mock: worker thread ran the frame loop
[PASS] mock: xrWaitFrame loop off the main thread (worker=8552 main=28416)
[PASS] mock: no frame-loop errors
[PASS] mock: full session-state transition sequence
  transition: UNKNOWN->IDLE @frame 0 (idle)
  transition: IDLE->READY @frame 5 (ready)
  transition: READY->SYNCHRONIZED @frame 10 (synchronized)
  transition: SYNCHRONIZED->VISIBLE @frame 15 (visible)
  transition: VISIBLE->FOCUSED @frame 20 (focused+steady)
  transition: FOCUSED->VISIBLE @frame 40 (focus-lost)
  transition: VISIBLE->FOCUSED @frame 50 (focus-restored+recenter)
  transition: FOCUSED->STOPPING @frame 80 (stopping)
[PASS] mock: stereo frames rendered (50 rendered)
[PASS] mock: focus-loss/idle frames unsubmitted (25 unsubmitted)
[PASS] mock: headset-absent frames gated (10 gated, backend untouched)
[PASS] mock: frame time monotonic across absence+recovery (first=11111111 last=833333325)
[PASS] mock: recenter applied on the worker at focus restore
[PASS] mock: LOCAL/STAGE/VIEW locate every phase
[PASS] mock: per-eye FOV asymmetric in scene
[PASS] mock: per-eye tint readback verified (20 verified, 0 failures)
[PASS] mock: controller injection round-trips on the worker
[PASS] mock: LOCAL reflects head pose before recenter
[PASS] mock: recenter re-baselines LOCAL to identity head pose
[PASS] mock: shutdown stops the backend
[PASS] mock: clean shutdown ordering
ALL TESTS PASSED
```

Real pass (exit 0, this machine, no headset streaming):

```text
  instance_created=1 system_acquired=0 requirements_queried=0 device_created=0 session_created=0 session_begun=0 stage_available=0 focus_received=0 session_loss=0 instance_loss=0 frames_pumped=0
  runtime="VirtualDesktopXR" v1.0.10 state=UNKNOWN
  failure="xrGetSystem (HMD) failed: XR_ERROR_FORM_FACTOR_UNAVAILABLE (headset absent or unreachable; degrading to diagnostics)"
[PASS] real: startup fails cleanly with no headset
[PASS] real: not running after failed startup
[PASS] real: diagnostics name the failing step (...)
[PASS] real: shutdown safe after failed startup
REAL_BACKEND_EXERCISED: instance=1 (degraded before session; stereo scene rendering proven on mock, nothing faked)
REAL_BACKEND_NOT_EXERCISED: system, D3D11 requirements, device, session, spaces, swapchains, xrWaitFrame frames, READY/FOCUSED transitions, loss handling (all gated behind a streaming headset)
ALL TESTS PASSED
```

Real-backend steps exercised vs not (no headset reachable):

- Exercised: loader load, `xrCreateInstance` (runtime identified as
  VirtualDesktopXR v1.0.10), `xrGetSystem` failure path, graceful
  degradation (`running()==false`, named failure, safe shutdown).
- NOT exercised (gated behind a streaming headset): system acquisition,
  D3D11 requirements query, device creation, session, spaces, swapchains,
  `xrWaitFrame` frames, READY/FOCUSED transitions, recenter request path,
  loss handling, stereo scene rendering through the real backend. If a
  headset streams, the same exe runs the full real path instead
  (worker frames + recenter + tint readback + shutdown ordering); that run
  has not been observed on this machine — recorded here, not faked.
- `third_party/openxr` reused as-is (headers + lib + dll); nothing rebuilt.

## Boundaries / deviations

- None from the T9 brief. No game interaction whatsoever; no CMakeLists,
  existing-file, git, or `build/` changes (build outputs went to
  `%TEMP%\mecvr-t9`; four stray `.obj` files the direct `cl.exe` call
  dropped in the repo root were deleted).
- No STOP triggered (S1–S5 clear: no unknown builds, no hooks, no
  camera/stereo-before-M2 game work, all tests green, no Frosty launch).
