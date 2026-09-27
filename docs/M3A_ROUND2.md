# M3a Round-2 Analysis Record

- Log: `%TEMP%\mecvr_m3a_23148.log`, 21.4 MB, 220584 lines, presents 1..16675.
- Capture duty cycle: 256-dump budget refills every 600 presents (~25% duty).
  Windows below are 600-present blocks (w0 = p0-600 ... w27 = p16200-16675).
- id=2 (1136B, matw=8, 386373 Maps, 95547 fp changes) is the ONLY CB that
  changes every present. Only id=2 is dumped (7424 dumps, all id=2).

## Consensus track (idx0, full-RT off32) per window

- w0-w9: dead static eye (37.9,7.5,1190.3), cumR <= 1.1 deg.
- w10-w13: static eye, monotonic yaw drift ~17-24 deg/window (NOT sweeps).
- w14: static eye, monotonic pitch 16.3 deg.
- w15-w19: near-static (cumR 1-8.5 deg), sim-time rate dips to 0.60-0.66.
- w20: eye jumps to (59.1,1181.7), static within; w21: (82.4,1172.2) static
  within; w22: drift (101.9->103.1); w23: static (103.1); w24: static eye +
  real ~89 deg basis rotation near yaw-wrap; w25: drift to (95.9);
  w26-w27: static (95.9,1.2,1156.9).
- Within-present main-eye spread: 0.00 in ALL quiet windows; 17-66u mean
  (max 77-1143u) in w20/21/24/25 = multi-camera interleaving proven there.

## Decided findings

1. off96 = main projection: m00=0.546211 m11=1.30484 (ratio 2.3889 =
   3440/1440 exact), near 0.06, infinite far, per-present TAA jitter on
   m02/m12 only (identical across all main passes of a present).
2. off416/528/640 = static axis-swap matrices (distinct=1 all log);
   off960 = static params (0.85, 20000, 16, ...). Only dynamic slots:
   off0 (clock), off32 (view), off96 (proj jitter).
3. off0 = (t, t+78.54, t, 0), monotonic game-time clock. Rate 1.00/s in
   w0-w9, dips 0.60-0.66 in w15-w18, 0.8-0.9 late. Epoch candidate WITH
   sim-rate caveat (not wall time).
4. RT reuse of id=2: 3440x1440 x3156, 1720x720 x3435 (same eye+proj as
   full = half-res chain), 0x0/vp512 x769 (OWN eye + OWN proj, e.g.
   m00=m11=1.92/7.6; VS/PS 8219E238/3F49D0B8 locked 93.5%), 256x256 x64
   (ONLY in w20/21/24/25, wild eyes = tertiary/loading passes).
5. Pass reuse: 134 distinct VS/PS pairs on id=2; half-res VS pair recurs
   (8217DFF8/84574FB8 = fixed post/downsample shaders).
6. Orthonormality: max residual 1.87e-06 over all 7424 off32 (solve valid).
7. ExecuteCommandList: execl=0 in ALL status lines. Deferred path ABSENT;
   slot-58 pass proxy gets nothing. Pass attribution must use VS/PS + RT.
8. ids 1/18/32/44: ~1.2M Maps each but fp changes only every ~5 presents,
   collapse in w25+ (unload/quit). NOT per-frame cameras. Never dumped
   (size unknown, probe cap 1MB). Small static CBs 10/11/105-108 appear
   mid-log (streaming boundaries p5822/p6860/p7552-7724/p8852).

## Gate verdict: ISOLATION PROTOCOL FAILED

No 80s span shows still(20s static) -> yaw(large oscillating yaw, static
eye) -> pitch(large pitch, static eye) -> walk(continuous translation).
Observed vocabulary (static holds, <=24 deg monotonic drift, teleport
jumps 21-1143u, menu/quit chaos) matches NONE of the run-card phases.
Pre-walk "teleports" appear WITHOUT any walk input -> under the run
orders, this is evidence AGAINST the isolation, not against the solve
(the solve itself verifies: orthonormality 1.9e-06, aspect exact).

## Fork (needs user testimony + round-3 probe)

- id=2-fullRT-off32 IS the displayed scene's camera (8-pass full+half
  structure + TAA proj proves it). It did not follow the run inputs.
- Prime suspects: (A) inputs never reached the game (focus loss; user
  misattributed drift/teleports as response); (B) a hidden camera path
  (`UpdateSubresource`, which is now audited for structured SRV uploads).
- Discriminator: user testimony (did the image respond? menus? T0?) +
  round-3 probe: phase markers, focus guard, UpdateSubresource audit.

## Testimony resolution (user, same session)

- Image responded to mouse; one perf dip (10-20 s, recovered); no loading
  screens; tabbed in after inject, closed pause menu, uninterrupted
  stopwatch-timed 20 s phases. Tutorial start; walk crossed 2 gates/doors;
  end-turn caused by a wall, then walked a bit more.
- Mapping: pause menu + tab-in = w0+ setup; perf dip = streaming hitches at
  the gates (sim-time rate dips to 0.6-0.84 = fixed-step time loss).
- Walk strobe (static windows + ~23u gap jumps) = GATE RHYTHM: walk 20u
  segments fall in dark gaps, door-waits fall in capture windows. The jumps
  are walked distance (3.2 u/s x ~7 s), not teleports. No pre-walk teleport
  exists: w19/20+ jumps are all within/after the walk phase.
- Muted yaw/pitch (monotonic <=24 deg drift, never sweeps) = tutorial
  scripted look-magnetism dragging the camera to objectives while eating
  user sweeps. Monotonic (script target) not oscillating (user) fits.
- VERDICT: location contaminated (scripted cameras, gates, walls), not
  effort. The run card's setup bar (no triggers/cutscenes) was unmeetable
  at the tutorial start. Round-3 re-runs the SAME probe (no rebuild) at a
  clean post-tutorial open rooftop. If textbook signatures appear, E6
  confirms and the gate can close on main-view authorship. If muted AGAIN,
  escalate to the hidden-path theory (UpdateSubresource audit probe).

## Correction (user): motions were small/slow, tracking exact

- User: screen moved exactly with input; slow small left/right looks, then
  slow up/down. No damping, no magnetism - the script theory is discarded.
- Consequence: w10-w13 yaw slices (yawR 17-24 deg, pitchR <0.9 deg) and the
  w14 pitch slice (pitchR 16.3 deg, yawR 0.6 deg) ARE the run phases,
  fragmented by the 25% duty cycle. 2 s windows catch single-direction
  slices of ~6-12 s sweep periods, so cumR~=range is EXPECTED, not
  evidence of monotonic drift. Axis purity (yaw-only then pitch-only,
  matching card order) cannot be script drift.
- Revised verdict: E6 (id2-off32 = gameplay main view) is SUPPORTED by
  round-2: exact on-screen tracking + axis-pure phase slices in the main
  upload. Small magnitudes = small user motions, not attenuation.
- Round-3 remains: clean zone (no gates/hitches/walls) + BIG motions so
  oscillation (cumR >> range) survives the 25% duty cycle unambiguously.

## No-capture forensics (v5/v6, same log, no new run)

- Block layout (id=2, 1136B): off0 sim-clock (t, t+78.537, t, 0);
  off16 RT+inverse; off32 main view; off96 main proj; off224/352
  secondary proj (menu/quit camera, appears only p14429+); off416/528/640
  static face matrices; off736 small-cam eye+params (0x0/512 passes only);
  off960 static params; off976 static camera params (1,1,0,0, 0.785=pi/4,
  far 20000, 16); off1072 color-ish params; singletons at 176/192/304/320
  in rare 256x256 dumps. rows0 lanes 8-15 redundantly confirm off32 rows.
- Clock fit: slope 0.01149/present (0.862 x realtime overall), intercept
  71.2 sim-s (level loaded before attach), maxres 5.3 s (hitches/menus).
  y-x const 78.5371 (not exactly 5890/75); z-x exact 0. Epoch candidate
  WITH sim-rate caveat stands.
- id=1: fp changes 95% on a 4-present cadence (18.75 Hz metronome, main
  RT). Not a camera; independent subsystem clock (exposure/DR-like).
- TAA jitter: 1007 distinct pairs over 1542 presents (~32x32 = 1024-cell
  lattice, 5-bit subpixel quantization). 46% of values repeat but ALL
  repeat lags >2000 presents, zero back-to-back; not f(present),
  not short-period. Lattice order unknown (open question, not
  gate-critical). No frame counter recoverable from jitter.
- off96 present in all full/half/0x0 dumps, absent in all 64 256x256 dumps.

## Gate scorecard (round-2 close)

1. Main view candidate: CONFIRMED (off32 full-RT consensus + testimony).
2. Projection candidate: CONFIRMED (off96 decoded + params + menu-cam).
3. Pass reuse across RTs: CONFIRMED (full/half/512/256x256 + VS/PS map).
4. Previous/current: NOT FOUND (no prev slot; jitter unordered).
5. Epoch/generation: PARTIAL (off0 clock + id1 4-beat; no frame counter).
6. Shared-vs-main: CONFIRMED (shared block, main/secondary/tertiary
   mapped, late menu camera proves time-multiplexing).
- Round-3 still needed for: textbook pre-walk stillness + continuous walk
  + another look at prev/epoch under clean conditions. If those stay
  absent, escalate to an observation-only probe retarget (dump id=1/18/32/44
  content) - no Map/Unmap writes, M3a rules intact.
