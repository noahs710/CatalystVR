# CatalystVR changelog

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
