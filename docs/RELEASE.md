# MECVR Sub-project 1 Release Scope (T12, test stream)

Date: 2026-09-26. This is a SUB-PROJECT scope note, not a product
release. Nothing here is shippable to players; "shippable" below means
usable by the next sub-project's engineers as proven foundation.

## Shippable now

- **Diagnostics**: M0A static/process baseline (`BASELINE_M0A.md`),
  thread-safe file logger (`src/diagnostics/logging.*`), live
  support-bundle builder (`src/diagnostics/support_bundle.*`,
  CLI `mecvr_support.exe --bundle --out DIR`). The bundle is a plain
  directory + MANIFEST.txt — no zip library vendored in T12 scope;
  zip the folder with any tool.
- **Observer**: synthetic Present/ResizeBuffers hook with
  state-preservation proofs (`OBSERVER_M0C1.md`); live transparency
  proof on retail 1.0.3.47248 (`OBSERVER_M0C2.md`,
  `HOOK_EVIDENCE.md` E1–E3, E5); capture-point identity
  (Present-time backbuffer = final output, 9929/9930).
- **Mock transport**: `MockXRBackend` (deterministic timing,
  trajectories, record/replay), bounded frame mailbox
  (`src/openxr/mailbox.h`), mono-frame CPU carrier
  (`src/render/m2b_mono.*`), XR frame worker
  (`src/openxr/xr_frame_worker.*`), standalone stereo scene on mock
  (`XRSCENE_M1C.md`).
- **Input interfaces**: taxonomy, per-family arbitration, binding
  schema, OpenXR/XInput skeletons with neutral-until-test behavior
  (`INPUT.md`, 62 checks green).
- **Compatibility scanner**: best-effort known-signature scan with
  clean live self-scan (`COMPATIBILITY.md`).
- **Tests**: ctest 7/7 green (2026-09-25 23:17, `LastTest.log`);
  per-stream assert-exes documented in their milestone docs.

## Explicitly NOT included

No camera discovery or manipulation. No stereo rendering of game
content (the M1C scene is standalone test content, not a game hook).
No gameplay input wiring (only Recenter/diagnostics/test paths are
meaningful). No HUD conversion or pre/post-HUD separation
(`HOOK_EVIDENCE.md` E4 unresolved by design for M2). No live XR
transport proof (T10G gate + T11 live rows open — headset required).
No Frosty-coexistence proof, no Trial-launch evidence, no packaging.

## Limits for consumers

- Retail 1.0.3.47248 evidence only; Trial must be fingerprinted and
  adapted independently (S1) — Trial was never launched.
- Scanner findings are best-effort: a clean scan never proves a clean
  process.
- Support bundles record gaps in MANIFEST.txt; a missing artifact is
  an absent artifact, never a clean bill.
- M3 camera work is out of scope and must not start from this record.
