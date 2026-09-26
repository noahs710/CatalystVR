# Real OpenXR Backend (M1B, plan T8)

Standalone `RealOpenXRBackend` implementing `IXrBackend` against the
Khronos loader with `XR_KHR_D3D11_enable`. No game device, no game thread,
no camera/stereo/gameplay work (STOP S3 respected). Owned by the render
stream; lives in `src/openxr/` (new files only — mock files untouched).

## Files

- `src/openxr/real_xr_backend.h` / `.cc` — `RealOpenXRBackend` + the
  `RealBackendDiagnostics` bring-up record.
- `src/openxr/real_xr_test.cc` — standalone validation exe (see below).
- `third_party/openxr/` — minimal SDK vendoring (see next section).

## SDK source + revision

- Source: sparse git clone of
  `https://github.com/KhronosGroup/OpenXR-SDK-Source`, branch `main`.
- Commit: `3ed64d0f9bb680f24b80a085091e5c8fab38f7b7`
  (2026-09-15, "hello_xr: Fix incorrect printf format").
- The nested clone `.git` was removed after vendoring; the hash above is
  the permanent source record. Re-fetch with
  `git clone --depth 1 --filter=blob:none --sparse <url> openxr` and
  `git sparse-checkout set` for the paths below, then verify
  `git rev-parse HEAD` matches.

## What was kept under third_party/openxr and why

- `include/openxr/` — the six API headers the backend compiles against.
  `openxr_platform_defines.h` is checked in upstream; the other five
  (`openxr.h`, `openxr_platform.h`, `openxr_loader_negotiation.h`,
  `openxr_reflection*.h`) are *generated* from `specification/` at SDK
  configure time (OpenXR 1.1.63) and were copied from the out-of-repo
  build dir. Kept because the backend needs them and they are not in git.
- `lib/openxr_loader.lib` + `bin/openxr_loader.dll` — the built Khronos
  loader (Release, MSVC 19.51), the only binary the backend links/loads.
  (The `.exp` was a link byproduct and was not kept.)
- `src/loader/` + `src/common/` + top-level `CMakeLists.txt` + `LICENSE` —
  loader source reference and rebuild provenance. Everything else
  (`specification/`, tests, samples, docs) was dropped after the build.
- Build location: the loader was configured and built **outside the repo**
  at `%TEMP%/openxr-loader-build` (`-DBUILD_TESTS=OFF
  -DBUILD_API_LAYERS=OFF -DBUILD_ALL_EXTENSIONS=OFF -DDYNAMIC_LOADER=ON`,
  `--target openxr_loader`, Release). No `build/` directory exists or may
  be created inside `third_party/openxr`.
- Build host needed two codegen prerequisites installed with
  `python -m pip install --user`: `pyparsing` then `jinja2` (SDK header
  generation fails without them: `ModuleNotFoundError` in the
  `generate_openxr_header` step). Worth knowing for any rebuild.

## Behavior (real-API mapping)

`startup()` runs the full bring-up on the calling thread and returns false
at the first failure, leaving a precise `failure_reason`:

1. `xrCreateInstance` with `XR_KHR_D3D11_enable`, `apiVersion = 1.0`
   (not `XR_CURRENT_API_VERSION`: the installed VDXR runtime rejects a 1.1
   request with `XR_ERROR_API_VERSION_UNSUPPORTED`). Runtime name/version
   recorded. Optional `XR_FB_display_refresh_rate` function pointers
   resolved (Key Decision 4).
2. `xrGetSystem` (HMD). `XR_ERROR_FORM_FACTOR_UNAVAILABLE` (headset absent)
   degrades gracefully to diagnostics — the normal no-headset path.
3. `xrGetD3D11GraphicsRequirementsKHR` adapter-LUID + min-feature-level
   gate (Key Decision 1).
4. Own D3D11 device (`D3D11_CREATE_DEVICE_BGRA_SUPPORT`) created on the
   DXGI adapter matching the runtime-required LUID (Key Decision 10).
5. `xrCreateSession` with `XrGraphicsBindingD3D11KHR` on our device.
6. LOCAL + VIEW spaces always; STAGE only where
   `xrEnumerateReferenceSpaces` advertises it, else LOCAL fallback
   (Key Decision 9).
7. Stereo view configs (`xrEnumerateViewConfigurationViews`, must be 2)
   plus one color-attachment swapchain per eye; images enumerated as
   `XrSwapchainImageD3D11KHR`.
8. Event pump (`SESSION_STATE_CHANGED` → `xrBeginSession` on READY /
   `xrEndSession` on STOPPING, loss/exit flags, `REFERENCE_SPACE_CHANGE`
   recreates spaces) runs inside `waitFrame`, i.e. on the caller's
   dedicated XR worker thread (Key Decision 3). Before READY,
   `waitFrame` returns `should_render=false` without calling the runtime
   frame functions — gating, not faking.
9. `endFrame` submits zero layers in M1B (scene layers arrive with M2B);
   `controllerState` is neutral (no action set yet — grip/boolean actions
   via `xrCreateActionSpace` arrive with the T4 input wiring);
   `displayFrequencyHz` prefers the FB extension rate, else the
   `xrWaitFrame`-derived EMA cadence.
10. `recenter()` posts a LOCAL re-baseline request applied on the worker
    inside `waitFrame` (space recreate). Session loss / instance loss are
    latched in diagnostics; `shutdown()` is safe after failed startup and
    releases in reverse order.

## Validation

Compiled directly with MSVC, zero warnings (`/W4 /WX` clean):

```bat
VsDevCmd.bat -arch=x64
cl.exe /nologo /W4 /WX /EHsc /std:c++17 /I src /I third_party\openxr\include ^
  src\openxr\real_xr_backend.cc src\openxr\real_xr_test.cc ^
  src\openxr\mock_xr_backend.cc ^
  /link third_party\openxr\lib\openxr_loader.lib d3d11.lib dxgi.lib ^
  /OUT:%TEMP%\mecvr-t8\real_xr_test.exe
```

Run with `third_party\openxr\bin` (the loader DLL) on `PATH`. Test output
(exit 0, this machine, no headset streaming):

```text
main thread=19076
  instance_created=1 system_acquired=0 requirements_queried=0 device_created=0 session_created=0 session_begun=0 stage_available=0 focus_received=0 session_loss=0 instance_loss=0 frames_pumped=0
  runtime="VirtualDesktopXR" v1.0.10 state=UNKNOWN
  failure="xrGetSystem (HMD) failed: XR_ERROR_FORM_FACTOR_UNAVAILABLE (headset absent or unreachable; degrading to diagnostics)"
[PASS] real: startup fails cleanly with no headset
[PASS] real: not running after failed startup
[PASS] real: diagnostics name the failing step (...)
[PASS] real: shutdown safe after failed startup
REAL_BACKEND_EXERCISED: instance=1 (degraded before session; no frames pumped, nothing faked)
[PASS] mock: startup
[PASS] mock: full 120-frame lifecycle on worker (120 rendered)
[PASS] mock: frame loop off the coordinating thread
[PASS] mock: recenter re-baselines LOCAL
[PASS] mock: controller injection round-trips
[PASS] mock: stereo view count
[PASS] mock: display frequency
[PASS] mock: shutdown stops the backend
ALL TESTS PASSED
```

Real-backend steps exercised vs not (no headset reachable):

- Exercised: loader load, `xrCreateInstance` (runtime identified as
  VirtualDesktopXR v1.0.10), `xrGetSystem` failure path, graceful
  degradation (`running()==false`, named failure, safe shutdown).
- NOT exercised (gated behind a streaming headset): D3D11 requirements
  query, device creation, session, spaces, swapchains, `xrWaitFrame`
  frames, READY/FOCUSED transitions, loss handling. These stay open until
  VDXR + headset stream; the full lifecycle meanwhile is proven on
  `MockXRBackend` (120 worker-thread frames, spaces, recenter, input).

## Boundaries / deviations

- `apiVersion 1.0` instead of current: required by the installed runtime
  (documented above); revisit if VDXR gains 1.1+ support.
- `controllerState` neutral and `endFrame` zero-layer until input wiring
  (T4) and scene layers (M2B) land — recorded here, not hidden.
- SDK tree keeps `src/common` though only `src/loader` is compiled: it
  holds headers the loader build references and stays as source
  provenance per the vendoring brief.
