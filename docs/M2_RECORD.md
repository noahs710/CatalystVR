# MECVR Provisional M2 Record (T12, test stream)

Date: 2026-09-26.

**Status: `M2 PASS`**

M2 passed on live-headset proof 2026-09-26 (VDXR + Quest 3, retail
Catalyst pid 33012, VIEW-space quad build). All 11 live items green
(see "M2 PASS — live proof" below); no camera/stereo/timing/gameplay
hooks exist or were used. This state is frozen as the M2 transport
baseline — no further transport changes unless later evidence
demonstrates a real transport defect.

## Per-item verdicts (spec §Sub-project 1 milestone detail)

| # | Gate | Verdict | Evidence |
|---|------|---------|----------|
| 1 | P1.0 Foundation (repo, CMake/toolchain, logging, config, tests) | PROVEN | `GAME_BUILDS.md` (MSVC 14.51.36231, cmake 4.4.3, git 2.54.0, VDXR active); ctest 7/7 green 2026-09-25 23:17 (`build/Testing/Temporary/LastTest.log`) |
| 2 | M0A Baseline characterization | PROVEN | `BASELINE_M0A.md` (retail+Trial hashes 1.0.3.47248, live retail process survey, clean exit) |
| 3 | M0B Safe injection (bounded worker, zero graphics hooks, identical behavior) | PROVEN | `INJECTION_M0B.md` (pid 30756, heartbeats, 60 s watch, game dir byte-identical) |
| 4 | M0C1 Synthetic DX11 observer | PROVEN | `OBSERVER_M0C1.md` (ALL CHECKS PASSED: install, state preservation, bit-identical unhook) |
| 5 | M0C2 Live DX11 observation (device, swapchain, cadence, capture, LUID) | PROVEN | `OBSERVER_M0C2.md` (3440x1440 exclusive fullscreen, ~200 fps single thread, capture BMP verified, LUID `0x00000000:0x0001a699`) |
| 6 | M1A MockXR backend | PROVEN | `MOCKXR_M1A.md` (21/21 deterministic checks) |
| 7 | M1B Real OpenXR session | SPLIT — bring-up + graceful degradation PROVEN; session→frames PENDING-HEADSET | `OPENXR_M1B.md` (VDXR v1.0.10 named, `FORM_FACTOR_UNAVAILABLE` handled, 120-frame lifecycle on mock) |
| 8 | M1C OpenXR test scene (real + mock) | SPLIT — mock full pass PROVEN; real scene PENDING-HEADSET | `XRSCENE_M1C.md` (50 stereo frames, tint readback, session-state sequence on mock) |
| 9 | M2A Frame capture (final buffer identified; pre/post-HUD prove-or-mark) | PROVEN (final output) + E4 explicitly unresolved | `HOOK_EVIDENCE.md` E1 (high), E2 (S2 clean), E3, E5; E4 unresolved with falsification test — M2 needs transport only |
| 10 | T10G Integrated adapter/LUID gate | PENDING-HEADSET (gate open) | Game LUID recorded (`OBSERVER_M0C2.md`); gate binary `tests/m2b/t10g_gate_main.cc` exits OPEN without a headset — never faked to PASS |
| 11 | M2B Mono submit (game frame → XR, stable, desktop usable, no camera hooks) | PENDING-HEADSET (live row open) | Headless transport proven by construction: `src/render/m2b_mono.*`, `src/openxr/mailbox.h`, `src/openxr/xr_frame_worker.*`, `tests/m2b/m2b_mono_test_main.cc` (identical eyes, newest-wins, non-blocking publish, S3 attestation); code-level `kM2bTouchesGameCamera=false` |

Support: input interfaces + compatibility scanner are proven
scaffolding (`INPUT.md`, `COMPATIBILITY.md`); full per-row detail in
`TEST_MATRIX.md`; sub-project scope in `RELEASE.md`.

## Pending physical-HMD proof block (single block, all headset-gated)

1. VDXR + streaming headset: real session, spaces, swapchains,
   `xrWaitFrame` frames on the XR worker (T8 remainder).
2. T10G live comparison: Catalyst LUID vs
   `xrGetD3D11GraphicsRequirementsKHR`, integrated session on match
   only (T10G gate).
3. M2B live: mono MEC frame stable in headset, desktop usable,
   recenter works, HMD motion decoupled from the desktop game camera
   (T11 live row) → M2 PASS.

## Test + STOP notes

- ctest: 10/10 green (foundation, input, mock XR, real-XR degraded,
  scene, M2B transport, matrix/soak, gate, synthetic observer,
  conflict scanner). T11 additions: mismatched-rate matrix
  (200/60/90/45 over 90), stall/resume, shutdown-with-pending, 60k
  soak with exact accounting; eye-log ring capped at 256 (was
  unbounded — fixed at integration).
- STOP S1–S5: none triggered. One worker-scheduler stall (not a code
  failure): a subagent blocked forever on a shell call the scheduler
  queued but never started; canceled and recovered directly. Permanent
  rule: workers get PowerShell-native commands only, bounded waits, no
  long unattended live runs — live-game/HMD validation stays
  user-driven.
- Hard gates honored: no game-directory writes, unknown builds fail
  closed, bounded shutdown only, no hot unload.

## Live-headset gate procedure (run unchanged build first — no fixes
## before baseline evidence)

1. Quest awake, VDXR confirmed as the active OpenXR runtime.
2. Start Catalyst normally (working dir = game dir), attach MECVR via
   the validated injector path.
3. T10G live: `xrGetD3D11GraphicsRequirementsKHR` LUID + feature level
   vs recorded Catalyst LUID `0x00000000:0x0001a699` / FL 11.1.
4. Catalyst frame visible in-headset.
5. Identical mono content both eyes; no per-eye camera offset.
6. HMD move/rotate: desktop Catalyst camera completely unchanged.
7. Headset-relative motion is compositor reprojection only.
8. Record: Present rate, XR cadence, frame age, copy cost, reuse/drop
   accounting, mailbox high-water, XR wait/acquire/release timing,
   late frames.
9. Focus loss: remove headset / switch away / restore; session
   recovers without blocking Catalyst.
10. Game exit: bounded XR-worker/mailbox shutdown, no D3D11 state
    failures, no sustained Present regression.
11. Final support bundle; flip status to `M2 PASS` only if all pass.

On failure: stay inside M2, diagnose transport/session only — never
compensate with camera, stereo, timing, or gameplay hooks. On success:
tag/freeze this state as the M2 transport baseline (regression
reference: 5.5 us hook overhead, 0/9930 backbuffer mismatches,
state_fail=0) BEFORE any Sub-project 2 work. Next plan scope:
M3–M6 only (camera discovery → rotation → positional 6DoF →
asymmetric per-eye projection → same-epoch dual-pass stereo).

## Addendum 2026-09-26 — baseline run + convergence fix (stays inside M2)

Baseline (projection layers, build pre-quad): 781 frames submitted, 0
upload failures, 0 state failures, ~12 Hz submit, ~135 ms frame age —
image VISIBLE in Quest 3 but convergence wrong / very dizzying.
Diagnosis: identical pixels through two IPD-offset projection frustums
disagree per eye; plus 77 ms bilinear upload starving the submit rate.
Fix (transport presentation only, no camera/stereo/timing hooks): mono
image now goes to ONE compositor quad in LOCAL space (2.5 m, ~90°),
runtime renders each eye's view natively; chain sized to source for a
1:1 row-copy upload (~2 ms). Default quad; `MECVR_MONO_LAYER=projection`
restores the old path for A/B. First quad build silently fell back to
projection (SAMPLED/TRANSFER_DST usage flags rejected by VDXR; found via
up_ns still ~60 ms): fixed to COLOR_ATTACHMENT_BIT-only matching the
proven eye chains, plus max-extent clamp, nearest-fill fallback, and
`layer=`/`end_fail=` status fields. Live: `layer=quad 3440x1440
end_fail=0`, up ~0.7 ms, 71.5 Hz submit, age ~51 ms, 0 failures.
Status unchanged (still pending live proof); re-run the 11-item
procedure above on the quad build.

## M2 PASS — live proof (2026-09-26, VIEW-space quad build)

Two-step injection (openxr_loader.dll, then mecvr_m2b_live.dll) into
retail Catalyst pid 33012; VDXR active; Quest 3 connected.

Final `m2b-final` line (`%TEMP%\mecvr_m2b_33012.log`):

> presents=3531 captures=3531 skipped_thr=5678 state_fail=0
> submitted=3531 reused=18609 superseded=0 empty=0 upload_fail=0
> high_water=1 rate_game=65.6 rate_xr=72.0 age_last=59.3ms
> age_max=63.4ms up_ns=0.49ms layer=quad 1280x720 space=view
> end_fail=0

(Note: `quad 1280x720` — the game downsized its swapchain during the
close sequence; the quad resize-rebuild path held live with zero
failures. Steady-state run was `quad 3440x1440` at 71.6 Hz, 0.7 ms
upload, ~54 ms age.)

11-item verdicts: (1) VDXR session/spaces/swapchains/waitFrame green.
(2) T10G live: `T10G_GATE_VERDICT: PASS` (exit 0) — runtime requires
LUID 0x0:0x1a699 = recorded Catalyst LUID, min FL 11.0 satisfiable by
game 11.1, integrated session created on the Catalyst adapter
(VDXR v1.0.10). (3–5) Stable
identical mono in-headset, both eyes same frame. (6) Desktop game
camera completely unchanged under HMD motion. (7) Headset-relative
motion is compositor presentation only. (8) Metrics above; mailbox
high-water 1; no late-frame pathology. (9) Focus/exit degradation
clean (presents froze, worker re-showed last frame, zero failures).
(10) Bounded worker shutdown via named event (`m2b-final` written);
process exited; `state_fail=0 upload_fail=0 end_fail=0`; game dir
byte-identical (337 files, max write 2026-06-15T09:37:04Z — never
written). The game ignores WM_CLOSE (exclusive fullscreen), so worker
stop was signaled directly before process termination (M0B precedent).
(11) This record + support bundle.

Human visual confirmation (all four): panel centered straight ahead
at eye level; correct ultrawide aspect; image stable and comfortable
(no dizziness); desktop camera unaffected by HMD movement; panel
stays centered while turning the head. Head motion does NOT move the
game camera — correct M2 behavior (tracking is Sub-project 2).
