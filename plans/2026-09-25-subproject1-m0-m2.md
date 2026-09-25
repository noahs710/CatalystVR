# MECVR Sub-project 1 Execution Plan — Foundation / Loader / OpenXR / DX11 Observer / Input Interfaces (M0–M2)

## Goal
Build Sub-project 1 of MECVR: repo foundation, compatibility-first loader, OpenXR core, DX11 observer, and input interfaces — proving the chain Catalyst → DX11 → MECVR → OpenXR frame loop → headset in mono (M2 PASS), with zero camera, stereo, or gameplay hooks.

## Success Criteria
- M2 PASS is true: MEC image reliably visible in headset; no camera manipulation, no stereo hack, no gameplay hooks.
- Every milestone gate P1.0–M2B (spec §"Sub-project 1 milestone detail") met with recorded evidence.
- `ctest` fully green; support report generated from a live game process; vanilla and Frosty launches behave identically to unmodded with VR disabled.

## Context And Current Facts
- Design spec: `specs/2026-09-25-mecvr-design.md` (Approach C, runtime conversion layer, evidence-gated hooks, M6 same-simulation-epoch gate, per-family input arbitration, Balanced comfort default).
- Game inventory (verified): Frostbite `.sb/.toc` layout (`Data/Win32` + `Patch/Win32` mirrors, `ui.sb` 9,935 bytes, `levels/` = `sp` only), `MirrorsEdgeCatalyst.exe` (87 MB) + Trial exe, `NvCameraSDK64.dll`, offline build (`CPY.ini`, AppID 1026480). Game dir is a read-only reference — never written.
- `Documents/MECVR` currently holds `specs/` and `plans/`; it becomes the repo root at P1.0.
- Environment (verified this run): cmake 4.4.3, git 2.54.0, active OpenXR runtime = Virtual Desktop Streamer (VDXR). MSVC `cl` is NOT on PATH — P1.0 resolves it.
- Architectural precedent only: titanfall2vr (Northstar plugin + DX11/camera hook + gamepad-emulation aim + per-element HUD fixes). Its Source-engine hooks do not transfer; its layer shape does.

## Constraints And Non-goals
- Non-goals: camera discovery, stereo rendering, gameplay input wiring, HUD conversion, comfort, hands, packaging, release. Any such work appearing before M2 PASS triggers STOP S3.
- Unknown/unrecognized exe builds fail closed for invasive hooks (retail and Trial independently hashed, fingerprinted, adapted).
- No DLLs placed beside the game exe; no game-asset modification; never reorder user Frosty mods; hot unload is optional (bounded shutdown only).

## Key Decisions
1. Standard OpenXR via the Khronos loader with `XR_KHR_D3D11_enable`; adapter LUID and minimum feature level taken from `xrGetD3D11GraphicsRequirementsKHR`, not assumed.
2. `xrWaitFrame` is authoritative for XR frame scheduling ONLY (predicted display time + interval flow through the XR pipeline; called once per frame on the XR worker). XR frame pacing is owned by MECVR; Catalyst Present cadence is measured, never modified. When game and XR cadences differ, M2 submits the newest safe completed Catalyst frame available, duplicating/dropping/reusing captured frames as needed. No hooks to Catalyst clocks, simulation timing, frame limiter, or camera exist in Sub-project 1; any MEC timing integration needs a later plan amendment.
3. A dedicated XR frame worker owns all OpenXR calls (`xrWaitFrame`, `xrBeginFrame`, `xrLocateViews`, swapchain acquire/wait/release, `xrEndFrame`, event/session-state processing). Handoff to the DX11 observer is a bounded frame-mailbox/copy-request queue. The Present hook NEVER calls `xrWaitFrame` and never waits unboundedly — stalling Frostbite's render thread on XR timing is forbidden. The game render thread may perform the required D3D11 copy at a known safe point on the game immediate context, but must never wait indefinitely for XR. Ownership is documented for: OpenXR calls (XR worker), game D3D11 immediate context (game thread only), XR swapchain textures (XR worker), capture textures (observer, shared read-only with XR worker via fence), synchronization primitives, and shutdown ordering (XR worker drains mailbox, then observer unhooks, then bounded shutdown). No D3D11 multithread protection and no cross-thread immediate-context use without explicit evidence and measurement.
4. Refresh: enumerate via `XR_FB_display_refresh_rate` where exposed and offer only runtime-exposed rates; otherwise derive the XR target cadence from `xrWaitFrame` (MEC timing untouched).
5. Observer hooks `IDXGISwapChain::Present` / `ResizeBuffers` (+ render-target/depth binding only where necessary) with full D3D11 state preservation; inspectors and frame dumps before any stereo attempt.
6. Input interfaces defined now per revised spec §12–§13 (layered taxonomy, `ActionState` with source/timestamp/pressed/released/held, pose validity separate, per-family arbitration with threshold + hysteresis, `InputMode`/`DominantHand`/`TurnMode`/`MovementReference` enums, binding schema, OpenXR + XInput skeletons); only Recenter, diagnostics, and test inputs are wired. Core config loader/writer + versioning + schema infrastructure is frozen in T1; the input stream owns only `src/input/**`, `src/config/input_bindings.*`, and INPUT.md.
7. Gamepad route uses XInput (Microsoft notes GameInput as the superset successor; XInput is the scoped choice for Sub-project 1).
8. CMake + CTest build and test; MSVC assumed pending P1.0 verification. `/W4` + warnings-as-errors for first-party MECVR targets; never force `/WX` on vendored third-party code.
9. Spaces: LOCAL primary, STAGE where available, VIEW for head-locked fallback content; grip/aim spaces via `xrCreateActionSpace` for the input skeleton.
10. Integrated XR sessions require the D3D11 adapter/LUID gate: Catalyst's DXGI adapter/LUID must match `xrGetD3D11GraphicsRequirementsKHR` (adapter + feature level) or the integrated path fails cleanly with both adapters logged. Cross-adapter texture sharing is out of scope for M0–M2.

## Recommended Approach
P1.0 foundation first, then parallel evidence tracks (baseline characterization, DX11 observer, input interfaces, MockXR), converging at safe injection (M0B), real OpenXR (M1B), test scene (M1C), frame capture (M2A), and mono submit (M2B). Five workstreams with exclusive file ownership; synchronization reviews at M0B, M1C, and M2 PASS.

## Work Plan
Owners are parallel-subagent workstreams; files are exclusive per stream.

- T1 — P1.0 foundation. Owner: launcher stream. Files: `CMakeLists.txt`, `build/` scripts, `.gitignore`, `src/`, `tests/`, `tools/`, `third_party/`, logging + frozen core config interface (loader/writer, versioning, section registry for later modules), CTest harness. Deps: none. Validation: `git init` + initial commit; `cmake -S . -B build -A x64`; `cmake --build build --config Release`; `ctest --test-dir build -C Release` with at least one foundation smoke test proving test-exe generation, CTest discovery, Release config, and runtime linkage (never a zero-test pass); `/W4` + `/WX` on first-party targets; resolve MSVC (`cl` via Developer Prompt or install Build Tools C++ workload) and record toolchain versions in `docs/GAME_BUILDS.md`.
- T2 — M0A static/process baseline (no D3D11 object claims). Owner: game stream. Files: `src/bootstrap/`, `src/compatibility/`, `docs/GAME_BUILDS.md`. Deps: T1. Validation: `MECVR.exe --diagnostics` against running MEC produces the static section of the support report — exe path/name, hash, file/product version, retail-vs-Trial identity, process/window info, client-area resolution, fullscreen/window state where externally observable, loaded-module enumeration, known overlay/injector/proxy presence, launcher/Frosty process relationship. Retail and Trial hashed/fingerprinted/identified independently; a live graphics report is required only for builds actually executed, and M2 is never blocked by unlaunched on-disk executables.
- T3A — M0C1 synthetic DX11 observer. Owner: render stream. Files: `src/render/`. Deps: T1. Validation: Present/Resize hook implementation + state-preservation tests green on a synthetic D3D11 harness; no live-game dependency.
- T3B — M0C2 live DX11 observation (after M0B injection). Owner: render stream. Files: `src/render/`. Deps: T6. Validation: actual D3D11 device/context, candidate/primary swapchain, HWND, DXGI format/size/mode, measured Present cadence, ResizeBuffers behavior, frame capture/dump; populates the live-graphics section of the support report. Never claim live D3D11 facts before this observer measures them.
- T4 — Input interfaces + skeletons. Owner: input stream. Files: `src/input/**`, `src/config/input_bindings.*`, `docs/INPUT.md` (core config interface frozen by T1 first). Deps: T1. Validation: unit tests for arbitration (threshold/hysteresis ownership, held-source ownership, family isolation), `ActionState` transitions, config schema load; no MEC wiring except Recenter/diagnostics/test inputs.
- T5 — M1A MockXR backend. Owner: render stream. Files: `src/openxr/` (`MockXRBackend`). Deps: T1. Validation: mock poses/views/controllers/timing drive the test scene without a headset; trajectory replay deterministic.
- T6 — M0B safe injection. Owner: game stream. Files: `src/bootstrap/`, `mecvr_runtime.dll`. Deps: T2. Validation: DLL loads into live MEC with logging/IPC up, bounded worker lifecycle, ZERO graphics hooks enabled initially, identical game behavior (spot-check + log), clean process exit; Frosty-launched attach path demonstrated.
- T7 — Compatibility scanner (best-effort conflict detection, never exhaustive). Owner: game stream. Files: `src/compatibility/`, `docs/COMPATIBILITY.md`. Deps: T2. Validation: enumerates loaded modules, recognizes known proxy/overlay modules, inspects expected DXGI function addresses/prologues where safe, reports suspicious detour ownership/anomalies; warnings never block unless real instability is observed (then STOP S2). Never claim definitive detection of every unknown Present hook.
- T8 — M1B real OpenXR session (standalone backend). Owner: render stream. Files: `src/openxr/` (`RealOpenXRBackend`). Deps: T1. Validation: instance → system → D3D11 requirements query → session → LOCAL/STAGE/VIEW spaces → swapchains → `xrWaitFrame` loop on the dedicated XR worker against VDXR; headset-absent path degrades gracefully to diagnostics; session loss/focus loss/recenter handled. No game device involved yet.
- T9 — M1C OpenXR test scene. Owner: render stream + test stream. Files: `src/openxr/test/`, `tests/`. Deps: T5, T8. Validation: standalone stereo scene renders on real AND mock backends.
- T10 — M2A frame capture. Owner: render stream. Files: `src/render/`. Deps: T3B. Validation: final game color buffer identified; pre/post-HUD boundaries recorded in `HOOK_EVIDENCE.md` (or marked unresolved with a falsification test).
- T10G — Integrated D3D11 adapter/LUID gate. Owner: render stream. Files: `src/render/`, M2 evidence record. Deps: T3B, T8. Validation: Catalyst's DXGI adapter/LUID vs `xrGetD3D11GraphicsRequirementsKHR` (adapter + feature level) compared; integrated XR session created ONLY on match; on mismatch, clean failure logging both adapters/LUIDs with a plain-language GPU-mismatch explanation. No cross-adapter sharing in Sub-project 1.
- T11 — M2B mono submit. Owner: render stream. Files: `src/render/`, `src/openxr/`. Deps: T8, T10, T10G. Validation: game frame copied into the XR projection layer via the bounded mailbox handoff, stable submit, desktop remains usable; no camera/gameplay hooks exist or are enabled (code-level assertion); no Catalyst camera matrices/transforms written by MECVR; moving the HMD does not change the desktop game camera; pre-composition capture is HMD-independent. The XR compositor may still reproject the submitted mono image normally — M2 proves transport, not head-controlled rendering.
- T12 — Matrix + docs close-out. Owner: test stream. Files: `docs/TEST_MATRIX.md`, `docs/RELEASE.md` (sub-project scope), support-bundle tool. Deps: T6, T9, T11. Validation: `MECVR.exe --support-bundle` reproduces all milestone evidence; M2 PASS checklist signed.

Execution order: P1.0 foundation (T1). Parallel: M0A static characterization (T2) + M0C1 synthetic observer (T3A) + input interfaces/schema (T4) + MockXR (T5). Then: M0B safe injection (T6). Then parallel: M0C2 live observer + compatibility scan (T3B/T7) alongside M1B standalone backend (T8). Then: M1C test scene (T9) + M2A capture (T10). Then: adapter/LUID gate (T10G) + M2B mono submit (T11). Then: support bundle, matrix, M2 PASS record (T12). Sync reviews at M0B, M1C, and M2 PASS.

STOP conditions: S1 unknown build → fail closed, no invasive hooks. S2 Present-hook instability (hitch/crash/regression vs baseline) → halt, diagnose, no escalation. S3 any camera/stereo/gameplay-hook work before M2 PASS → out of scope, revert. S4 red unit tests (math, arbitration, scanner) → fix before advancing. S5 Frosty-launched game fails under observer → compat investigation only.

Hard gates (unchanged, all required): no game-directory writes; no camera work before M2; no stereo work before M2; no gameplay input wiring before M2; unknown build fails closed; bounded shutdown only with no hot DLL unload requirement; Frosty coexistence investigation on any regression; all unit tests green before advancement; M2 = Catalyst → DX11 → MECVR → OpenXR → headset using the same mono game image for both eyes.

Deliverables: repo (`/docs /src /tests /tools /third_party` + this plan + spec), binaries (`MECVR.exe`, `mecvr_runtime.dll`, `oxr_test_scene`), foundation docs (ARCHITECTURE, HOOK_EVIDENCE, GAME_BUILDS, OPENXR, INPUT, COMPATIBILITY, TEST_MATRIX + sub-project RELEASE), one live support report, green `ctest` log, M2 PASS record.

## Validation Plan
- `cmake -S . -B build -A x64` then `cmake --build build --config Release` then `ctest --test-dir build -C Release` — expected: clean configure, foundation smoke test passes (proving exe generation, discovery, Release config, linkage), all tests pass with `/W4` + `/WX` clean on first-party targets.
- `MECVR.exe --diagnostics` (vanilla + Frosty launch) — expected: static support report (hashes, modules, resolution, overlays) at M0A; live-graphics section (device, swapchain, cadence) only after T3B measures it; game behavior identical.
- `oxr_test_scene.exe --backend mock` then `--backend real` — expected: stereo scene on both; session events handled.
- Manual (headset required, VDXR active): M2B shows stable mono MEC frame; desktop usable; recenter works; moving the HMD does not change the desktop game camera while compositor reprojection of the submitted image behaves normally.
- Highest-risk check: T3A/T3B/T6 Present-hook stability on the live Frostbite frame loop (S2 gate).

## Risks / Rollback
- MSVC absent from PATH → T1 installs/locates Build Tools (C++ workload) or uses an existing Developer Prompt; no compiler substitution without a plan amendment.
- Headset unavailable at T8/T11 → MockXR covers logic; real-backend gates stay open until VDXR + headset present.
- Retail-vs-offline-build divergence → per-build adapters; Trial never borrows retail evidence.
- Render-thread hazards → bounded shutdown only; no `FreeLibrary` hot unload (spec §2).
- Rollback for every milestone: unload DLL / exit process; uninstall is deleting the MECVR tree; game dir untouched by design.

## Open Questions
None. Assumptions recorded instead: VDXR + Quest headset available for T8/T11 (MockXR otherwise covers); user plays via EA App or equivalent launcher for Frosty-path testing; MSVC install permitted at T1 if missing.

## Sources
- https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrWaitFrame.html
- https://registry.khronos.org/OpenXR/specs/1.0/man/html/XrGraphicsRequirementsD3D11KHR.html
- https://registry.khronos.org/OpenXR/specs/1.0/man/html/XR_KHR_D3D11_enable.html
- https://registry.khronos.org/OpenXR/specs/1.0/man/html/XR_FB_display_refresh_rate.html
- https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrCreateActionSpace.html
- https://registry.khronos.org/OpenXR/specs/1.0/man/html/XrReferenceSpaceType.html
- https://github.com/KhronosGroup/OpenXR-SDK-Source
- https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present
- https://learn.microsoft.com/en-us/windows/win32/xinput/xinput-game-controller-apis-portal
- https://cmake.org/documentation/
- https://learn.microsoft.com/en-us/cpp/build/building-on-the-command-line?view=msvc-170
- https://github.com/TinyBlkDog/titanfall2vr
