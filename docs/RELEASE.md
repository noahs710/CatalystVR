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
- **Display-mode policy**: pure, fail-closed resolution/refresh resolver
  with explicit `0` kill-switch and unit coverage (`src/render/force_mode.*`).
- **Opt-in M3b camera bridge**: separate `mecvr_m3b_live.dll` target with
  one-snapshot OpenXR pose handoff and guarded retail view-matrix rotation;
  the observe-only M3a probe remains the default capture tool.
- **Motion scheme**: deterministic STRIDE-style arm-swing locomotion and
  gesture mapping in `src/input/motion_scheme.*`, with stick fallback.
- **Full-body pose foundation**: a deterministic 21-joint humanoid solver owns
  spine, arms, legs, idle gait, locomotion, crouch, airborne and climb states.
  It is live-produced from OpenXR tracking, retains analog trigger/grip values
  for authored hand animation. When the runtime exposes
  `XR_FB_body_tracking` plus `XR_META_body_tracking_full_body`, the optional
  84-joint provider supplies authoritative joints into the same solver;
  unsupported or partial tracking falls back per joint to procedural IK.
  It remains disconnected from the retail skeleton until the palette adapter
  is proven.
- **Physical crouch calibration**: the first valid standing head sample anchors
  the session floor plane; head lowering can therefore select crouch/slide
  states, and recentering starts a fresh calibration.
- **Mod-owned IK overlay**: opt-in `MECVR_ENABLE_BODY_OVERLAY=1` renders the
  current solver pose as composited triangle geometry with torso, limbs, hands,
  tracked-hand-orientation weapon silhouettes, articulated five-finger-plus-thumb
  grip-driven hands, view-space depth/perspective shading, and half-IPD eye
  separation during temporal stereo capture,
  trigger-edge shot pulses, muzzle flashes, and trigger/grip response into
  the final D3D11 target. Projection is head-local so the body and weapons
  remain aligned while the player turns. It does not depend on a Catalyst
  skeleton ABI and is covered by headless geometry and WARP renderer tests.
- **Motion-authored parkour poses**: a shared deterministic parkour-intent
  classifier drives distinct climb, vault, slide, and wall-run presentation
  states from tracked grip/hand-height/velocity signals. It remains a
  mod-owned contract and does not synthesize Catalyst's game-derived actions.
- **Authored firing recoil**: trigger edges add a bounded rearward impulse to
  the solved elbow/wrist/hand chain, then recover smoothly while preserving
  controller aim and replay determinism.
- **Skeleton discovery**: opt-in `MECVR_DISCOVER_PALETTES=1` diagnostics scan
  bounded mapped SRV buffers for 3x4/4x4 bone palettes. The expensive discovery
  path is disabled in normal play and performs no writes.
- **Native XR input**: optional OpenXR action set with per-hand grip spaces,
  trigger/squeeze/menu/face buttons, and thumbsticks, sampled from the XR
  worker and neutral on unsupported runtimes.
- **Gameplay bridge**: opt-in foreground-gated keyboard/mouse synthesis maps
  arm-swing movement, sprint, crouch, jump, turn, and trigger clicks to the
  existing game controls. Fast tracked-hand strikes also produce a
  debounced primary-attack click when triggers are not held;
  `MECVR_ENABLE_INPUT=1` is required. The calibrated physical-crouch signal
  also drives the game's crouch key when input synthesis is enabled, and both
  hands raised above the head produce one debounced jump tap. The launcher
  exposes persisted toggles for the jump gesture and physical-crouch gameplay
  bridge; disabling either leaves mod-owned body animation active.
- **Optional parkour gameplay bridge**: the shared intents can additionally
  synthesize vault/slide/climb desktop controls (Space/Ctrl/E) through an
  explicit launcher toggle; its scan codes are launcher-configurable, it is
  disabled by default, and it never writes native Catalyst state.
- **Unified M3B live path**: camera pose publication, final backbuffer capture,
  and XR submission now share one bounded worker/session; camera mode no
  longer depends on a second injected XR module.
- **Comfort turn**: smooth and snap turn modes are implemented with tested
  threshold/re-arm/cooldown behavior; `MECVR_TURN_MODE=snap` selects snap.
- **Stereo gate**: independent eye buffers carry an explicit simulation epoch
  and pose sequence; mismatched or stale pairs are rejected, and the real
  backend switches from the mono quad to projection mode before acquisition.
  `MECVR_ENABLE_STEREO=1` (explicit launcher opt-in; the public alpha defaults
  to the correctly converged projection fallback) enables the experimental
  temporal producer: consecutive game presents use the located left/right
  camera poses and are paired for OpenXR submission. A simultaneous
  engine-native dual-pass producer remains the required final replacement.
- **Tests**: ctest 35/35 green (2026-09-26, Release configuration), including
  observer delivery of asymmetric mock eye views, the GPU-worker routing
  handshake, bounded shared-capture mailbox, center-crop blit math, and a
  two-device D3D11 shared-texture contract; per-stream assert-exes are
  documented in their milestone docs.
- **GPU transport**: camera mode capability-gates a triple-buffered D3D11
  NT-handle/keyed-mutex path. Catalyst performs one nonblocking GPU copy and
  the XR device center-crop blits directly into each runtime-owned eye image.
  CPU readback and duplicate per-eye CPU allocations are bypassed while this
  path is active. Any adapter, registration, resize, or mutex failure revokes
  readiness and returns to the established CPU fallback.
- **Launcher/package**: `tools/package_release.ps1` creates a tested ZIP;
  `launch_preview.ps1` starts the game in its install directory, injects the
  loader first, then the selected MECVR module.
- **Frosty coexistence**: optional, direct-by-default backend launches a
  configured Frosty pack and injects only into a newly matched exact Catalyst
  process; Frosty-owned files and profiles remain untouched.
- **Engine intelligence**: read-only Frostbite camera/input metadata and the
  current dual-pass evidence boundary are recorded in `ENGINE_INTEL.md`.

## Explicitly NOT included

No proven live per-eye game rendering yet (the M1C scene is standalone test
content, and the stereo gate is ready for a real dual-pass producer).
No HUD conversion or pre/post-HUD separation
(`HOOK_EVIDENCE.md` E4 unresolved by design for M2). No live XR
transport proof (T10G gate + T11 live rows open — headset required).
No Frosty-coexistence proof and no Trial-launch evidence. The preview ZIP is
reproducible, but it is an engineering preview rather than a finished player
release until the live per-eye producer and headset validation are complete.

## Limits for consumers

- Retail 1.0.3.47248 evidence only; Trial must be fingerprinted and
  adapted independently (S1) — Trial was never launched.
- Scanner findings are best-effort: a clean scan never proves a clean
  process.
- Support bundles record gaps in MANIFEST.txt; a missing artifact is
  an absent artifact, never a clean bill.
- M3 camera work is out of scope and must not start from this record.
