# MECVR Feature Benchmark

Benchmark target: the public Mirror's Edge (2008) VR mod at
<https://github.com/letsgosportsteam/mirrors-edge-vr-mod>. Its v0.2.2-alpha
README and release notes define the minimum bar. MECVR is not considered a
finished Catalyst conversion until every parity row is proven and the Catalyst
specific improvements are also proven.

| Area | 2008 mod baseline | MECVR current evidence | Release gate |
|---|---|---|---|
| Stereo | Native separate eye views | Experimental temporal eye pair | Simultaneous per-eye scene rendering, correct effects, no stale pair |
| Head tracking | Positional and rotational 6DoF | Guarded camera constant-buffer rewrite | Headset-proven 6DoF across gameplay, cinematics, death, menus |
| Motion hands | Controller-driven hands with animation handback | Canonical full-body pose plus analog trigger/grip data; opt-in mod-owned IK overlay; no retail skeleton consumer | Faith's visible arms/hands follow controllers with contextual handback |
| Body | Game-owned body/arms outside supported hand states | Mod-owned 21-joint humanoid solver, procedural gait/crouch/air/climb state | Game-visible full-body IK with calibrated proportions and collision-safe handback |
| Weapons | Two pistols, either hand, pickup/drop/throw/calibration | Trigger maps to mouse; no tracked weapon pose | Controller aiming, muzzle alignment, two-hand policy, pickup/drop/throw |
| Melee | Punch and two-hand disarm gestures | Basic controller input synthesis | Physical punch, kick/disarm intent, cooldown and contextual gating |
| Parkour | Ledge, pipe and bar grip interactions | Arm-swing run, raised-hands jump, crouch | Ledge/pipe/bar climbing, vault and wall-run gestures with safe game-state gating |
| UI/HUD | Stereo HUD, menus and prompts; adjustable scale/height | Desktop backbuffer submission | Stable binocular HUD/menu layer with depth, scale, height and reticle controls |
| Comfort | Smooth/snap turn, camera locks, reticle/lens controls | Smooth/snap turn and bounded camera translation | Pitch/roll locks, vignette, reticle, animation comfort profiles |
| Settings | In-headset saved settings panel | Native desktop launcher with persisted basic settings | Launcher exposes every setting; in-headset quick settings and restore defaults |
| Resolution/performance | Headset-derived resolution and frame cap | Force-mode policy exists; launcher lacks controls | Runtime resolution scale, refresh/frame cap, quality presets and live diagnostics |
| Runtime support | VDXR-only 32-bit path | Khronos OpenXR loader, 64-bit D3D11 path | Validated VDXR, SteamVR and Meta OpenXR with capability diagnostics |
| Distribution | Three-file copy install | Self-contained ZIP and GUI launch handoff | Signed/versioned installer or portable package, update/uninstall and notices |
| Diagnostics | Detailed log and bug-report guidance | Runtime logs, support-bundle tool, fail-closed telemetry | One-click support bundle with redaction and compatibility report |
| Release quality | Double reproducible build, frozen tested ZIP, hashes | CTest-gated package and SHA-256 | Reproducible dual build, clean-room package audit, retail and headset matrix |

## Catalyst-only differentiators

MECVR must exceed parity with full-body IK rather than isolated hands, a GUI
launcher with profiles and diagnostics, automatic compatibility checks for the
retail executable, broader OpenXR runtime support, native D3D11 stereo, and
mod-owned procedural animation driven by tracked motion.

## Current milestone evidence

- `full_body_ik_test` proves the deterministic body model, limb lengths, gait,
  crouch, airborne state, replay, tracking loss and mailbox behavior without an
  HMD.
- `mod_owned_integration_test` runs the no-HMD chain from synthetic tracked
  input through full-body IK, firing recoil, motion-clip sampling, stereo
  overlay geometry, and raised-hands jump intent.
- `parkour_intent_test` covers deterministic climb, vault, slide, and wall-run
  intent selection from tracked-motion fixtures.
- Parkour gameplay synthesis is an explicit opt-in bridge only; its default-off
  path keeps the mod-owned animation contract independent of desktop controls.
- `bone_palette_classifier_test` proves bounded 3x4/4x4 palette discovery and
  rejection of camera matrices and noise.
- `native_skeleton_adapter_test` proves the new read-only seam requires a
  stable, repeated palette layout from the same resource, fingerprint, and
  presentation frame sequence before verification; it does not write or infer
  retail bone semantics.
- `native_bone_map_test` proves executable, resource, layout, complete-arm, and
  joint-uniqueness checks gate native bone-map readiness. Unmapped torso/leg
  entries remain `-1` so Catalyst retains its animation for those regions.
- Retail diagnostics found that Catalyst's four mapped constant buffers do not
  contain a contiguous skeleton palette. An opt-in SRV discovery path identified
  small structured-buffer candidates and is disabled by default after use.
- No game-visible skeleton claim is made until a versioned bone map and guarded
  writer are proven.

## Source snapshot

The baseline was audited on 2026-09-26 from the upstream README, v0.2.2-alpha
release notes, example configuration, and release workflow. Re-audit upstream
before each MECVR release because the benchmark may improve.
