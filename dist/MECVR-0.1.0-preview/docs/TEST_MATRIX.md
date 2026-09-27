# MECVR Sub-project 1 Compatibility / Test Matrix (T12, test stream)

Date: 2026-09-26. Scope: every compatibility dimension the plan gates,
one row each, with a verdict backed by cited evidence.

## Status vocabulary

- `PROVEN` — closed headless on this machine with the cited evidence.
  Nothing is marked PROVEN/PASS without evidence.
- `PENDING-HEADSET` — needs a streaming VDXR headset; MockXR covers the
  logic meanwhile, but the live row stays open.
- `PENDING-ENVIRONMENT` — needs third-party software absent from this
  machine (Frosty, ReShade, 3Dmigoto, overlays). Never read as failure.
- `OPEN` — tool present, run not yet done. Honest backlog, not a gate
  failure.

STOP rules for this matrix: S1 unknown build fails closed (retail and
Trial fingerprinted independently, `BASELINE_M0A.md`); S2 any
Present-hook hitch/crash/regression vs baseline halts (no escalation);
S3 camera/stereo/gameplay work anywhere before M2 PASS reverts; S4 red
tests block advancement; S5 Frosty-launched regression goes to compat
investigation only.

## A. Launch paths

| # | Item | Verdict | Evidence |
|---|------|---------|----------|
| A1 | Vanilla retail launch + static/process diagnostics | PROVEN | `BASELINE_M0A.md` (pid 36780, 1280x720 windowed, 93 modules, 0 overlay findings, clean exit) |
| A2 | Vanilla retail + runtime injection, identical behavior | PROVEN | `INJECTION_M0B.md` (pid 30756, heartbeat ticks, 60 s identical-behavior watch, bounded shutdown, game dir untouched) |
| A3 | Vanilla retail + live observer, no regression (S2) | PROVEN | `OBSERVER_M0C2.md` + `HOOK_EVIDENCE.md` E2 (`state_fail=0` over ~18k Presents, cadence steady ~5.0 ms) |
| A4 | Frosty plain-launch + inject attach path | PENDING-ENVIRONMENT | Frosty absent on this machine (`INJECTION_M0B.md` Frosty check); `--wait NAME` attach path implemented, demonstrated equivalent = attach-by-PID |
| A5 | Frosty + ordinary heavy modpack + observer | PENDING-ENVIRONMENT | Frosty absent; explicitly not failed — modpack rows stay open per plan validation |
| A6 | Trial exe live launch + diagnostics | OPEN | Trial hashed from disk only, never launched (`BASELINE_M0A.md`); retail evidence never borrowed (S1) |
| A7 | Packaged launcher: exact-PID loader + M3B attach | PROVEN | 2026-09-26 bounded smoke: packaged `launch_preview.ps1`, camera+input enabled, pid 28440; M3B log confirms attach and calibrated camera mode |

Vanilla/Frosty launch parity is covered by T2/T6 evidence (A1–A2);
Frosty itself is absent, so A4/A5 wait on environment, not on code.

## B. Graphics observation / capture

| # | Item | Verdict | Evidence |
|---|------|---------|----------|
| B1 | Synthetic observer: hook install, state preservation, clean unhook | PROVEN | `OBSERVER_M0C1.md` (`observer_test.exe`, ALL CHECKS PASSED) + ctest `synthetic_observer_test` |
| B2 | M2A final-output capture point (Present-time backbuffer identity) | PROVEN | `HOOK_EVIDENCE.md` E1 (match=9929/9930, high confidence) |
| B3 | Pre/post-HUD boundary separation | OPEN (explicitly unresolved) | `HOOK_EVIDENCE.md` E4 (zero per-frame OMSetRenderTargets; falsification test defined; M2 transport needs final output only) |

## C. XR backends

| # | Item | Verdict | Evidence |
|---|------|---------|----------|
| C1 | MockXR deterministic backend (timing, trajectories, replay, recenter, input) | PROVEN | `MOCKXR_M1A.md` (21/21 checks) + ctest `mock_xr_test` |
| C2 | Real backend bring-up + graceful no-headset degradation | PROVEN | `OPENXR_M1B.md` (VDXR v1.0.10 identified, `FORM_FACTOR_UNAVAILABLE` → diagnostics, safe shutdown) + ctest `real_xr_session_test` |
| C3 | Real session → spaces → swapchains → `xrWaitFrame` frames vs VDXR | PENDING-HEADSET | `OPENXR_M1B.md` §NOT-exercised; full lifecycle proven on mock meanwhile (120 worker-thread frames) |
| C4 | Stereo test scene on mock | PROVEN | `XRSCENE_M1C.md` (mock full pass: 50 stereo frames, tint readback 20/20, session-state sequence) |
| C5 | Stereo test scene on real backend | PENDING-HEADSET | `XRSCENE_M1C.md` real pass degraded before session; same exe runs full path when a headset streams |

## D. Gates / live transport (M2 proof)

| # | Item | Verdict | Evidence |
|---|------|---------|----------|
| D1 | T10G integrated adapter/LUID gate (live comparison) | PENDING-HEADSET | Game side recorded (`OBSERVER_M0C2.md`: RX 9060 XT, LUID `0x00000000:0x0001a699`, FL 11.1); gate binary `tests/m2b/t10g_gate_main.cc` exits OPEN (not PASS) without a headset |
| D2 | M2B mono submit: stable game frame in headset, desktop usable | PENDING-HEADSET | Headless pieces only: `src/render/m2b_mono.*`, `src/openxr/mailbox.h`, `src/openxr/xr_frame_worker.*`, headless test `tests/m2b/m2b_mono_test_main.cc` |
| D3 | M2 PASS physical-HMD proof block (mono MEC image visible, HMD motion decoupled from game camera) | PENDING-HEADSET | The single open proof block; see `M2_RECORD.md` |

## E. Conflict environment (best-effort scanner, never exhaustive)

| # | Item | Verdict | Evidence |
|---|------|---------|----------|
| E1 | Scanner unit cases + clean live self-scan | PROVEN | `COMPATIBILITY.md` (17/17 self-test; live pid 10288: 104 modules, 0 known-hits, DXGI prologues byte-clean) + ctest `conflict_scanner_test` |
| E2 | ReShade-present game process | PENDING-ENVIRONMENT | ReShade absent; synthetic matcher cases (ReShade basenames) PROVEN in E1 |
| E3 | 3Dmigoto-present game process | PENDING-ENVIRONMENT | 3Dmigoto absent; same synthetic coverage as E2 |
| E4 | RTSS / Steam / Discord overlay-present process | PENDING-ENVIRONMENT | Overlays absent at baseline (0 findings = absence noted, not proof of cleanliness) |

Zero scanner findings means "no known signature matched", never "no
hooks exist" (`COMPATIBILITY.md`).

## F. Unit / integration tests

| # | Item | Verdict | Evidence |
|---|------|---------|----------|
| F1 | Full ctest suite green | PROVEN | Release build run 2026-09-26: 35/35 pass, including `motion_scheme_test`, `comfort_filter_test`, `camera_math_test`, `camera_harness_test`, `view_override_test`, `stereo_epoch_gate_test` (including the strict M6 same-scene/same-pose gate), `arm_ik_test`, `full_body_ik_test` (including partial authoritative-body merge), `parkour_intent_test`, `mod_owned_integration_test`, `body_overlay_geometry_test`, `body_overlay_render_test`, `bone_palette_classifier_test`, `native_skeleton_adapter_test`, `native_bone_map_test`, `native_pose_writer_test`, `m3a_slot_proof`, `force_mode_test`, `immersive_defaults_test`, `early_injection_test`, `shared_capture_mailbox_test`, `shared_blit_math_test`, and `shared_texture_contract_test` |
| F2 | Input arbitration / ActionState / binding schema (62 checks) | PROVEN | `INPUT.md` + ctest `input_tests` |
| F3 | Fail-closed resolution/refresh policy | PROVEN | `src/render/force_mode.*` + ctest `force_mode_test`; malformed values and kill-switch covered |
| F4 | STRIDE-style motion mapping | PROVEN | `src/input/motion_scheme.*` + ctest `motion_scheme_test`; swing, sprint, stick override, and melee cases covered |

## STOP assessment (T12)

None triggered. No unknown builds (S1); no hook instability (S2);
no red tests in the latest full run (S4); no Frosty launch available, no
vanilla regression observed (S5). Live headset and dual-pass producer rows
remain open by design.
