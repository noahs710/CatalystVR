# Sub-project 2 — Camera 6DoF + True Stereo (M3–M6)

Date: 2026-09-26. Status: approved in principle; revised per review,
proceeding to P2.0 without a further design-review cycle. Baseline:
tag `m2-transport-baseline` (commit `e179847`, M2 PASS).

## Goal

Turn the proven M2 mono transport into a true VR presentation:
authoritative Catalyst render-camera dependency-chain discovery →
HMD rotational tracking → full positional 6DoF → correct asymmetric
per-eye projection → same-simulation-epoch native dual-pass stereo.
When this plan is done, looking and leaning move the in-headset
viewpoint with correct stereo depth, the simulation still updates
exactly once per frame, and the desktop reflects the currently
rendered Catalyst camera.

## Success Criteria

- M3: rotating the HMD rotates the in-headset view (yaw/pitch/roll)
  via a validated main-scene render-camera override; sim behavior
  identical to M2 baseline (no locomotion/anim change).
- M4: anchored positional HMD delta (lean, crouch, lateral,
  look-behind) moves the render viewpoint with configured bounds;
  Faith's world/player transform never touched by head motion.
- M5: each eye's projection independently verified correct
  (asymmetric frustum from the same-snapshot live `XrView`,
  calibrated units-per-meter); dizziness-free on the
  previously-dizzy projection path.
- M6: native dual-pass stereo — two eye renders of ONE prepared
  scene, one simulation epoch, one XR pose snapshot (hard gate:
  never pass M6 on split-epoch eyes or split-snapshot eyes).
- All new hooks evidence-gated in `HOOK_EVIDENCE.md` (E6+); unknown
  builds fail closed; ctest green; game dir never written.

## Context And Current Facts

- M2 transport baseline (frozen): identical 3440×1440 frame to a
  head-locked VIEW-space quad at 71.6 Hz, 0.7 ms upload, zero
  failures; `docs/M2_RECORD.md` = M2 PASS; support bundle
  `%TEMP%\mecvr-support-20260926-010739`. Rollback floor: tag
  `m2-transport-baseline`.
- Hook machinery proven: shared-vtable `Present[8]`/`ResizeBuffers[13]`
  observation, `StateGuard` (`state_fail=0` sustained),
  `src/render/vtable_hook.*`, `src/render/observer.*`
  (`docs/HOOK_EVIDENCE.md` E1–E5; E4 pre/post-HUD still unresolved).
- HMD truth source proven: `RealOpenXRBackend::locateViews` returns
  per-eye pose + asymmetric FOV every tick; worker paces on
  `xrWaitFrame` (`src/openxr/`). OpenXR ties each located view set
  to one display time and returns the complete set together; each
  projection-layer view corresponds to the same located view
  set/order. VIEW space tracks the viewer origin/centroid; the spec
  recommends `xrLocateViews` for world-render view/projection data.
- Input taxonomy + per-family arbitration exist as interfaces and
  skeletons only (`src/input/`, `docs/INPUT.md`); gameplay wiring is
  sub-project 3 and stays untouched here. HMD pose for M3–M6 flows
  backend → worker → published snapshot → camera module, never
  through input devices, and the camera thread never calls OpenXR.
- `src/camera/` and `src/game/` are empty (`.gitkeep` only):
  greenfield for this plan. `src/config/` Registry is frozen.
- Governing spec: `specs/2026-09-25-mecvr-design.md` §§5–6, 8, 12,
  15–16, 23–24; milestone ladder M3 rotation → M4 6DoF → M5
  projection → M6 stereo; transform hierarchy World → Player Root →
  VR Body Origin → Game Camera Base → Comfort Filtering → HMD Local
  → Eye Pose.
- Permanent rules from M2: PowerShell-native bounded commands, no
  long unattended live runs, live-game/HMD validation stays
  user-driven; two-step injection (loader DLL first); stop the game
  before relinking.

## Constraints And Non-goals

- Constraints: retail `MirrorsEdgeCatalyst.exe` 1.0.3.47248 only
  (Trial needs its own independent intel cycle later); evidence-gated
  hooks, semantic signatures, fail-closed unknown builds (spec §1,
  §21); the XR worker remains the only thread making OpenXR calls;
  zero XR work on the Present thread; bounded mailbox/worker
  architecture unchanged; Balanced-comfort default untouched.
- Non-goals (preserved scope gates): gameplay input, hands, comfort
  tuning (channels are separated here, never tuned), HUD extraction
  (E4 stays unresolved), final cutscene handling, dynamic
  resolution, refresh automation, packaging, unrelated M2 transport
  rewrites. Allowed M2 transport changes remain only fixes backed
  by demonstrated transport defects.

## Key Decisions

1. **M3a discovers the whole render-camera dependency chain, not
   one VP matrix.** A shader-visible VP that follows the screen is
   not automatically the authoritative camera. M3a classifies, where
   present: current view / projection / VP; inverse view / inverse
   projection / inverse VP; previous-frame view/projection/VP;
   camera world position; camera forward/right/up; near/far
   representation; TAA/projection jitter; jittered vs unjittered
   matrices; frustum/culling camera; visibility/LOD inputs;
   motion-vector camera history; main-scene vs shadow / reflection /
   UI / cubemap passes; render-view IDs or equivalent pass markers —
   using frequency, viewport/RT identity, draw ranges, timing,
   matrix relationships, player motion, and controlled camera
   motion. Deliverable: an evidence chain from game/render camera
   state → scene/culling preparation → main-scene camera constants
   → shader-visible view/projection → final game image. If the
   upstream CPU render-camera structure is identifiable, it is
   recorded read-only even though M3b still overrides render-side
   only. Rejected: calling E6 "authoritative" from a single
   screen-following VP (spec §5 forbids settling for less).
2. **Primary discovery vector: DX11 shader-constant observation.**
   Extend the proven vtable observation to the constant-buffer path
   (VS constant-buffer binds/updates) per Decision 1's taxonomy.
   Why: render-API truth, no memory-scan fragility, reuses proven
   machinery; the spec's prior survey (§17 + header) points at
   shader-level tractability. Rejected: blind float-pattern memory
   scans (build-noise prone); static-exe RE as the lead method
   (slow, no runtime truth). Fallback if CBs prove insufficient:
   targeted CPU-side struct discovery seeded by CB contents.
3. **One immutable XR pose snapshot for all M3–M6 camera work
   (non-negotiable).** Per XR frame the worker publishes one
   immutable `XRFramePoseSnapshot`: sequence, predictedDisplayTime,
   predictedDisplayPeriod, view-state validity/tracking flags,
   reference-space (recenter) generation, head/center pose, left
   `XrView` pose + FOV, right `XrView` pose + FOV, publication
   timestamp. The camera/render thread consumes snapshots only and
   never calls `xrLocateViews`. Left and right eye data for one
   rendered stereo frame MUST come from the same located view set /
   same predicted display time — never locate eyes independently.
   Snapshot age and sequence are tracked in diagnostics; on invalid
   tracking or staleness beyond the safety threshold, fail
   gracefully to previous-valid-pose / base-camera behavior, never
   apply garbage transforms.
4. **Render-side override, game memory read-only.** M3–M6 camera
   control lives in a new `src/camera/` layer overriding the
   view/projection the game submits for rendering; game structs are
   read-only intel (player root, body yaw/basis for the hierarchy).
   Why: keeps the simulation pristine (spec §§5–6 exit gates),
   fully reversible per frame, natural home for per-eye M6
   rendering. Falsification trigger: any required camera effect
   unproducible render-side (proven by evidence) gets its own
   E-entry and review — never a silent game-side write.
5. **VR Body Origin / recenter math is formalized before M3b, and
   absolute HMD pose is never applied directly.** At VR
   activation/recenter capture game camera/base orientation, HMD
   anchor orientation, HMD anchor position, and body/root basis
   where known; then apply HMD DELTA from anchor: game base camera
   × HMD delta from recenter = VR camera. M4 translation: (current
   HMD center − anchor HMD center) → meters→Catalyst-units →
   validated body/game basis → render-camera viewpoint only.
   Physical head translation never alters Faith's world/player
   transform. This keeps "VR forward" = Faith's current forward,
   never the runtime's startup forward. Tested in P2.0: arbitrary
   initial headset yaw, recenter, repeated recenter, body/game yaw
   changes, yaw wraparound, pitch/roll, translated LOCAL
   origins/reference-space changes.
6. **Projection conventions are discovered, not assumed.** Never
   assume textbook non-reversed finite D3D projection. P2.0/M3a
   determine and test: handedness, row/column-major storage,
   multiplication order, OpenXR +X/+Y/−Z mapping into Frostbite
   coordinates, D3D clip-space convention, normal vs reverse-Z,
   finite vs effectively-infinite far plane, projection jitter,
   near-plane representation, transposed/upload representations.
   The headless projection builder supports the observed Catalyst
   convention. M5 world scale is units-per-meter calibration; there
   is NO independent fake IPD scalar — runtime eye separation comes
   only from the actual left/right `XrView::pose` values (a measured
   IPD may be shown diagnostically).
7. **M3b is an engineering camera-control proof, not immersive
   projection yet.** Expected path: HMD rotation → immutable pose
   delta → validated main-scene render-camera override → Catalyst
   renders new orientation → M2 quad transports the result. The
   VIEW-space quad may remain as safe transport/fallback, but
   compositor movement of the quad is NOT M3 tracking: the matrix
   override itself is instrumented so evidence proves Catalyst's
   rendered camera changed from the HMD pose. The override enables
   ONLY for the validated normal-gameplay main camera/pass; on any
   ambiguity (cutscene, menu, reflection, shadow, unknown camera)
   it fails closed to the untouched game camera / M2 fallback. The
   later cutscene manager stays out of scope.
8. **M5 proves each eye independently without violating the view
   contract.** The full OpenXR view set is always represented; each
   submitted projection view retains its own pose/FOV from the same
   `xrLocateViews` snapshot, in view order. Diagnostic one-eye
   inspection renders/inspects that eye while the opposite eye is
   an explicit diagnostic blank/reference presentation — never a
   faked one-view stereo config, never one eye's pose/FOV fed to
   both views. Why: isolates projection math from dual-pass
   splicing so M6 failures can only be splice/epoch bugs.
9. **Same-epoch evidence starts in M3a; the token is chosen before
   M6.** M3a identifies candidate epoch evidence (sim frame
   counter, render-frame counter, scene-preparation generation,
   immutable render-world snapshot pointer/version, other directly
   evidenced frame identity). The epoch token is chosen and
   documented before M6 implementation starts. M6 pipeline: sim/
   update → scene preparation → capture immutable epoch N → left
   render tagged N → right render tagged N → XR submit. The gate
   proves: no sim update between eyes; no scene-prep generation
   advance between eyes; both passes carry epoch N; both use the
   same XR pose snapshot/predictedDisplayTime; only eye
   pose/projection differ. A hash may supplement, never be the sole
   proof when a direct generation token exists.
10. **M6 has a hard splice precondition.** No blind replay of D3D11
    commands between arbitrary boundaries, no whole-Present replay.
    Before writing the second-eye path, evidence must name a
    repeatable render entry/boundary that re-renders the prepared
    scene without advancing simulation or duplicating side effects.
    M6 pre-gate KNOWN: sim/update boundary, scene-prep boundary,
    render-scene/submission boundary, camera state consumed by that
    render, frame/epoch identity, which temporal/render state
    advances per render vs per sim frame. UNKNOWN = STOP and keep
    observing. The second eye is another render of the SAME
    prepared scene, not a second game frame. If the renderer cannot
    be invoked twice safely, stop with evidence and review the
    architecture — no silent command-stream replay, second Present,
    timer manipulation, or sim-suppression hacks. Spec Mode B
    (depth reconstruction) stays deferred exactly as planned.
11. **Temporal-camera state is tracked, not solved.** M3a
    identifies temporal consumers (previous matrices, jitter,
    motion-vector history) where possible; M3–M6 record whether
    they stay coherent. Never unknowingly overwrite current VP
    while leaving a required coupled constant inconsistent;
    document known mismatches; if an artifact blocks judging camera/
    stereo geometry, isolate that effect for validation rather than
    declaring the geometry bad. Permanent temporal-effect
    compatibility stays later scope.
12. **Desktop behavior.** From M3 on: "desktop reflects the
    currently rendered Catalyst camera." No final mirror promise
    yet: during M6 the desktop may show one eye or the game render
    target depending on the proven splice; final
    left/right/center/spectator mirror policy is the later
    mirror/polish milestone.

## Recommended Approach

Build in strict ladder order on the frozen baseline: headless camera
math + conventions + anchor/recenter math + fake-harness first
(P2.0), then an observe-only intel phase over the full dependency
chain plus epoch candidates (M3a) ending in a review gate, then
rotation delta (M3b), anchored translation (M4), same-snapshot
per-eye projection (M5), then the M6 pre-gate and the dual-pass
splice (M6). Each rung keeps all previous rungs green (quad mono
stays the fallback presentation until M6 replaces it) and adds
exactly one new live behavior with its own evidence entries and HMD
check. Nothing here tunes comfort, touches gameplay input, or
alters the sim.

## Work Plan

- **P2.0 Camera math + conventions + anchor/recenter + harness
  (headless, no game).** New `src/camera/`: OpenXR↔Frostbite/DX11
  coordinate conversion; matrix/quaternion helpers; convention-
  parameterized projection discovery support + asymmetric-projection
  builder from `XrView` (Decision 6 taxonomy); transform-hierarchy
  composer (spec §5 chain, comfort pass-through); VR Body Origin /
  anchor/recenter math (Decision 5 + full test list); immutable
  `XRFramePoseSnapshot` type + age/validity/staleness handling
  (Decision 3); units-per-meter calibration plumbing (no fake IPD);
  fake game-camera harness (spec §23: synthetic camera/HMD →
  expected eye matrices). New ctest suites. Depends on: nothing
  (baseline). Enables: everything.
- **M3a Observe-only camera/pass/culling/temporal/epoch discovery
  (live game).** Decision 1 taxonomy via constant-buffer
  observation: per-frame matrices + timing vs Present, pass
  classification (main/shadow/reflection/UI/cubemap), culling/
  visibility inputs, jittered-vs-unjittered pairs, motion-vector
  history, coupled temporal consumers (Decision 11); CPU
  render-camera structure recorded read-only where identifiable;
  epoch-marker candidates hunted (Decision 9). Deliverables:
  `HOOK_EVIDENCE.md` E6+ entries + `docs/CAMERA.md` intel sheet
  with the full evidence chain game state → scene/culling prep →
  main-scene constants → shader VP → final image. Zero writes.
- **M3a REVIEW GATE.** Authoritative main render-camera chain AND
  candidate epoch evidence must be proven before any override work.
  No M3b on a single screen-following VP.
- **M3b HMD rotational delta → validated main-scene camera
  override (live).** Decision 7 path with instrumented override
  proof; game projection untouched; mono quad retained as
  transport/fallback; main-pass-only enablement with fail-closed
  ambiguity. Gate: user look-around check + sim-behavior parity vs
  M2 baseline (locomotion/anim logs unchanged).
- **M4 Anchored positional HMD delta → render camera only (live).**
  Decision 5 translation path with configured bounds; head motion
  never drives world locomotion or Faith's transform; wall-clip =
  allow-with-bounds (fade/clamp is M11+ polish, spec §16). Gate:
  free-roam positional check + bounds/clip behavior demo.
- **M5 Same-snapshot per-eye `XrView` projection validation, one
  eye inspected at a time (live).** Decision 8 procedure with
  Decision 6 conventions and units-per-meter calibration; never
  ±IPD/2 fakes; projection layers return (the M2-dizzy path, now
  with correct same-snapshot per-eye frustums). Gate: per-eye
  geometry checks + projection-layer comfort check.
- **M6 PRE-GATE.** Decision 10 splice boundaries + Decision 9 epoch
  token proven and documented. UNKNOWN anywhere = STOP.
- **M6 Two eye renders, one prepared scene, one simulation epoch,
  one XR pose snapshot (live).** Decision 9 pipeline + Decision 10
  splice; quad mono retired to fallback (cutscene/cinema duty per
  spec §§7, 18). Gate: stereo depth check + epoch proof (all five
  sub-proofs) + full M3–M6 regression (rotation, translation,
  projection, comfort-neutral).

Revised execution ladder: P2.0 → M3a → M3a REVIEW GATE → M3b → M4 →
M5 → M6 PRE-GATE → M6. M6 PASS only when both epoch identity AND
stereo geometry are proven.

## Validation Plan

- P2.0: `ctest --test-dir build -C Release` (new `camera_math_test`
  + `camera_harness_test` suites); every matrix/projection/hierarchy/
  anchor/recenter/convention case headless and deterministic,
  including the Decision 5 test list and Decision 6 convention
  matrix.
- M3a: evidence review of E6+ entries + `docs/CAMERA.md` (full
  Decision 1 taxonomy cited, timing cited, falsification tests
  stated, epoch candidates named); no-behavior-change proof: M2
  baseline metrics reproduced (submit rate, age, zero failures)
  since nothing is written.
- M3b: HMD look-around (yaw/pitch/roll correct, no swim);
  instrumented proof the rendered game camera (not the quad) moved
  from the HMD delta; main-pass-only enablement demo incl.
  fail-closed on ambiguity; sim-parity: scripted in-game sequence
  produces identical animation/locomotion telemetry with override
  on vs off; ctest green.
- M4: HMD lean/crouch/lateral/look-behind moves the render
  viewpoint only; bounds respected; stick locomotion and Faith's
  transform unaffected by head motion; recenter/re-recenter
  correct under arbitrary yaw; ctest green.
- M5: left-inspected and right-inspected live geometry each match
  harness prediction within tolerance under the Decision 8
  procedure; same-snapshot proof (both views one
  predictedDisplayTime); asymmetric-frustum proof (no
  symmetric-projection + offset anywhere in code); no fake IPD
  scalar anywhere in the render path; projection-layer comfort
  check passes where M2-projection failed; ctest green.
- M6: stereo depth correct (near/far disparity check); epoch gate:
  all five Decision 9 sub-proofs green (direct generation token
  primary, hash supplementary at most); M3–M5 regression all
  green; support bundle regenerated.
- Every live step: two-step injection, bounded user-driven HMD
  sessions, `layer= / space= / end_fail= / upload_fail= /
  state_fail=` status line must stay clean; game dir re-verified
  byte-identical at M6 close.

## Risks / Rollback

- Frostbite wraps CBs unexpectedly (packing/encryption/obfuscation):
  likelihood low (M0C proves standard DX11 so far); fallback is
  Decision 2's CPU-side seeding; M3a gate stops the line before
  any write if intel is insufficient.
- Temporal/post-process effects smear under camera override (spec
  §22 flags Catalyst's heavy temporal stack): isolate and document
  per effect (Decision 11); fixing them is out of scope — M3–M6
  prove camera correctness, not effect compatibility.
- Render-thread timing too tight for per-frame override: measure
  first (M2 added 0.1% frame cost as reference); override is a
  matrix multiply + CB write, budgeted in microseconds.
- Dev motion sickness: short bounded HMD sessions (existing rule),
  comfort-neutral defaults, stop-on-dizzy protocol carried from M2.
- Rollback: every rung keeps the previous presentation working
  (env-gated); any failed gate rolls back to `m2-transport-baseline`
  behavior with zero transport edits — the tag is the floor.

## Open Questions

None. Retail-first ordering, render-side control, evidence gating,
the snapshot contract, anchor math, convention discovery, epoch
handling, splice preconditions, and the M3→M6 ladder are all settled
by the spec, the M2 record, and this review; remaining unknowns
(exact Frostbite camera structures, sim/render split points) are
execution discoveries with assigned methods, not plan questions.
