# MECVR Camera (Sub-project 2)

Scope: P2.0 headless camera math (this doc, present) + M3a live
render-camera intel (to be appended; E6+ entries live in
`HOOK_EVIDENCE.md`).

## P2.0 math ownership (`src/camera/`)

- `math.h/.cc` — double-precision Vec3/Quat/Mat4. Mat4 storage is
  row-major; vector convention is a per-call parameter, never global.
  Includes rigid + general 4x4 inverse, pose/view builders, YXZ
  test-only Euler extraction.
- `conventions.h/.cc` — parameterized projection/world conventions:
  mult order, clip-Z range, handedness, reverse-Z, infinite far,
  frustum tangents, constant-buffer pack order, units-per-meter +
  axis-map world conversion. No assumed Catalyst convention: M3a
  selects the observed one; the full 32-combo matrix is tested.
- `snapshot.h` — immutable `XRFramePoseSnapshot` contract (plan
  Decision 3): sequence, predicted times, tracking flags, space
  generation, head pose, both `XrView`s in locate order, publish
  time; usability/staleness/newness predicates.
- `body_origin.h/.cc` — VR Body Origin anchor/recenter math (plan
  Decision 5): capture HMD anchor + game reference; per-frame HMD
  delta composes with the LIVE game base; axis-map basis changes;
  generation-guarded graceful failure. Outputs a render-camera pose
  only — never a player transform.
- `hierarchy.h/.cc` — spec §5 chain composer (comfort is an explicit
  pass-through stage until M11); per-eye view via the same anchor
  evaluation as center (one code path, one snapshot, one units
  convention).
- `seam_convert.h` — float seam <-> double math converters.
- `harness.h/.cc` — fake game-camera harness (spec §23): scripted
  keyframes -> synthetic snapshots -> expected eye matrices. One
  code path for headless tests and later live M5 validation.

Tests: `camera_math_test` (core math, 32-combo convention matrix,
snapshot, Decision 5 anchor list), `camera_harness_test`
(snapshot builder, static/yaw/trajectory eyes, Faith-turns, lean
scaling, graceful-fail paths, hierarchy echo). ctest 12/12 green
(10 M2 + 2 P2.0).

Key verified facts: asymmetric off-center projections map edges to
exact NDC (sign verified vs D3DX off-center form); yaw wraparound
takes the short way; lean resolves through live game yaw; eye
separation is exactly IPD scaled by units-per-meter; there is no
fake IPD scalar anywhere (separation comes only from snapshot eye
poses).

## M3a live intel — round 1 (2026-09-26, pid 16552, gameplay)

Probe: m3a_probe.dll, 16 context slots (all headless-proven) +
Present/Resize. Zero writes. Phases: unset/menu, game_still,
game_yaw, game_pitch, game_walk, done. 12678 presents, 316 CBs,
6265 dumps, state_fail=0.

### R1-1: id=2 is an 1136-byte per-frame scene CB (HIGH confidence)

- Updated ~28x/present via Map/Unmap (game NEVER calls
  UpdateSubresource anywhere observed); binds=0 on the immediate
  context (binds live in deferred/command-list state — see R1-4).
- Layout (offsets in bytes): 0=time clocks (two, same rate);
  16=resolution (3440 1440 1/w 1/h, constant); **32=current VIEW
  matrix** (see R1-2); 736=direction-or-position + params (moves);
  960=static params (0.85, 20000, 16 — 20000 = far-plane
  candidate); 1072=color/params (motion-responsive last row).
- Seen across RTs 3440x1440 (main), 1720x720 (half-res effects),
  256x256, and null-RTV 512x512-VP passes.

### R1-2: id=2:off32 is a current VIEW matrix (HIGH confidence)

- Complete 4x4, row-major storage, column-vector form (bottom row
  exactly (0,0,0,1), translation in last column, columns
  orthonormal: col0 x col1 = col2 verified numerically).
- col1 ~= +Y (camera up); forward = -col2 (RH, camera looks -Z).
- Follows mouse look: still -> yaw rotates about Y; pitch phase
  shows pitched basis. Eye solved at (1186.5, 55.8, -123.3) in
  still (55.8-unit height consistent with rooftop gameplay;
  units-per-meter TBD in M5).
- NOT yet proven the authoritative MAIN view: falsification
  pending (per-pass value comparison shadow/reflection, projection
  pairing, update-timing vs main scene). E6 candidate, gate gated.

### R1-3: projection/VP NOT YET FOUND (open)

- The round-1 affine prefilter ((0,0,0,1) row/col) is blind to real
  view matrices with translation AND to projections (m15=0): only
  id=2 ever dumped (cands=1 of 316 is a filter artifact, not an
  intel result). Round 2 adds view/projection pattern tiers.
- Offsets 96-735 of id=2 unscanned-for-patterns (dumps exist only
  where the old prefilter hit): projection/VP/inverse/prev-frame
  may live there. Round 2 rescans with tiers.

### R1-4: deferred/command-list rendering (HIGH confidence)

- draws_last=0 across all 12678 presents: no Draw* issues on the
  immediate context; id=2 binds=0 despite 360K updates. Draws and
  binds execute from command lists. Consequences: (a) draw-range
  correlation unavailable on immediate (RT/VP/timing used
  instead); (b) CB slot/stage attribution blind for list-recorded
  binds — documented limitation, NOT a blocker: value + RT +
  motion analysis carries classification, and the M3b write vector
  (upload-content rewrite at Map/Unmap) does not need bind slots.
- Round 2 adds ExecuteCommandList observation (list-execution
  sequence ~= pass boundaries).

### R1-5: epoch tokens (early)

- DXGI frame counters flow every Present (PresentCount /
  LastPresentCount, e.g. 18115/18117 at detach): candidate #1 per
  plan preference (renderer frame ID class). CB-content counters
  (id=2 time clocks) noted as composite fallback only.
- Sim/render generation counters NOT YET observed; hunt continues
  in round 2 (fp-change timing analysis).

### R1-6: open questions for round 2

- Eye teleport: solved eye differs wildly between still-first and
  pitch-first samples (~1500 units) — user motion (walk/fall/
  cutscene between phases?) or misread? Asked user; round 2
  requires standing in one spot until the walk phase.
- Projection, VP, inverse, prev-frame, jittered pairs: undiscovered
  (prefilter fix in round 2).
- Culling/visibility inputs, motion-vector history, CPU-side
  render-view object: undiscovered.
- HMD-motion-vs-camera invariance: deferred (no headset).

## M3b opt-in rotational bridge (2026-09-26)

- New target: `mecvr_m3b_live.dll`. The existing `mecvr_m3a_probe.dll`
  remains observe-only and is unchanged by this path.
- With `MECVR_ENABLE_CAMERA=1`, M3b starts a dedicated OpenXR pose worker
  and publishes one immutable snapshot through `src/camera/pose_mailbox.h`.
  The render hook never calls OpenXR.
- The bridge recognizes only the observed 1136-byte scene constant buffer and
  rewrites only the view matrix at byte 32, once per resource per Present,
  using the anchor-relative HMD quaternion and a guarded orthonormality check.
  Projection and game/player state are untouched.
- Default behavior is disabled; invalid/stale snapshots, unknown buffer sizes,
  absent XR hardware, and malformed view data all fail closed to the original
  game matrix.
- Headless proof: `view_override_test` validates rotation composition,
  position preservation, and rejection of non-view buffers. Release CTest is
  15/15 green.
- Live smoke: retail `1.0.3.47248`, pid 3112, loader preload plus M3b
  injection, 2,166 Presents, `state_fail=0`, clean detach. VDXR reported
  `XR_ERROR_FORM_FACTOR_UNAVAILABLE`, so no HMD-driven override was applied;
  this is graceful-degradation evidence, not M3 rotation proof.
