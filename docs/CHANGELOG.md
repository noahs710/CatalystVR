# CatalystVR changelog

## 1.5.2.15-alpha — 2026-09-28

Temporal stereo runtime repair milestone:

- Stopped the temporal stereo eye phase from advancing when a Present was
  throttled or a D3D readback failed. This prevents left/right capture drift
  and the resulting non-convergent overlapping images.
- Removed the fixed 1920×1080 source bottleneck and raised the bounded source
  readback ceiling to 2880-class dimensions before the required square crop.
- Replaced the incorrect one-XR-tick epoch comparison with a bounded capture
  freshness gate. Game Present cadence and XR compositor cadence are
  independent; valid delayed pairs are now accepted while malformed or old
  pairs remain fail-closed.
- Added separate runtime counters for malformed, stale, backend-rejected, and
  upload-rejected stereo pairs, with those causes emitted in the transport log.
- Expanded pose history to preserve delayed pair identity across compositor
  ticks and added headless coverage for delayed producer epochs, freshness,
  stale-pair rejection, and timing jitter.

Known limitation: this release still uses the temporal per-Present capture
bridge rather than a native simultaneous dual-pass engine render. Native Faith
palette replacement remains evidence-gated and disabled until a stable retail
bone contract is captured.

Validation: focused stereo tests, full Release CTest, launcher/package
self-tests, and retail attach smoke are required before publishing.

## 1.5.2.14-alpha — 2026-09-28

Launcher and HMD-readiness milestone:

- Rebuilt the native launcher as a compact CatalystVR control center with a
  dark graphite surface, runner-red action hierarchy, readable two-column
  layout, grouped runtime controls, and a dedicated Advanced Settings panel.
- Added an explicit Quest 3 / VDXR HMD preset that selects immersive camera
  mode, stereo projection, tracked motion input, performance pacing, and the
  low-cost no-overlay default in one action.
- Added deterministic launch-readiness checks for the selected retail
  executable, packaged payload, OpenXR loader/M3B camera bridge, and the
  immersive profile. The launcher reports that the runtime checks the headset
  during startup instead of falsely claiming that a headset is connected.
- Preserved the existing persisted settings, dry-run validation, Frosty
  backend, motion clip controls, native Faith diagnostics, scan-code mapping,
  fail-closed validation, and PowerShell handoff.
- Added the launcher source directory to the build include surface and fixed
  Unicode/version rendering and strict-warning issues so Release builds remain
  warning-clean.

Validation: Release CTest, package self-test, HMD-profile dry-run, and retail
attach smoke are required before publishing this milestone.

## 1.5.2.13-alpha — 2026-09-28

Grouped motion/IK milestone:

- Added deterministic per-hand OpenXR intent for grip/fist, combat swings,
  and MAG-rope pull latching.
- Standard Quest Touch Plus squeeze/grip closes the corresponding mod-owned
  hand pose; the existing visible IK hand articulation follows that grip.
- Added gripped hand-swing combat pulses with direction, strength, cooldown,
  and one-shot latching so held fists do not spam attacks.
- Added grip-plus-pull-toward-body MAG-rope latching with release hysteresis,
  per-hand direction/strength, and configurable Q/ability bridging.
- Kept left-stick locomotion authoritative, with alternating tracked-hand
  swing fallback and right-stick turning unchanged.
- Changed jump synthesis from a tap to a held game action, allowing Catalyst
  to own ledge autograb/climb decisions while jump is held.
- Removed grip-driven climb synthesis from the mod-owned parkour classifier;
  vault/slide/wall-run presentation hints remain available.
- Added focused headless tests for fist closure, combat latching, one-hand
  MAG-rope activation, and game-owned climbing behavior.

Validation: Release CTest and launcher/package self-tests are required before
publishing this milestone.
