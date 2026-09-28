# CatalystVR launcher redesign

Date: 2026-09-28

## Goal

Turn the current native Win32 launcher into a public-release-quality
CatalystVR control center with a distinctive Mirror's Edge-inspired visual
language, a clear primary launch path, and an HMD readiness check suitable
for Quest 3 + VDXR testing.

The redesign must preserve the existing launcher executable, persisted
settings, PowerShell handoff, dry run, Frosty backend, motion clip controls,
native Faith contract diagnostics, and self-test contract. It is a presentation
and launch-safety improvement, not a runtime renderer rewrite.

## Visual direction

Use a dark graphite base, warm white text, restrained gray surfaces, and
runner-red accents for active states and primary actions. The window should
feel technical and athletic: crisp labels, generous spacing, short explanatory
copy, and strong hierarchy. No external UI framework, image asset, font
dependency, or network service is introduced; the launcher remains a single
native Win32 executable.

## Layout

The main window becomes a compact two-column control center:

1. Header: CatalystVR identity, version label, and a short “Quest 3 / VDXR
   ready” subtitle.
2. Target card: executable path, browse button, validation state, and a clear
   selected-game indicator.
3. Runtime card: immersive camera, stereo projection, motion input, runtime
   mode, performance profile, turn mode, and units-per-meter calibration.
4. HMD readiness card: four deterministic checks — game executable, package
   files, OpenXR loader, and immersive settings — with a single dry-run result
   and an explanation when a check fails.
5. Advanced card: parkour scan codes, physical gesture toggles, overlay,
   pacing, Frosty backend, motion clips, and native Faith diagnostics. This
   section is visually secondary but remains accessible without removing any
   existing capability.
6. Footer: primary “LAUNCH VR” action, secondary dry run/save actions, and a
   readable status console.

The window should fit comfortably on a 1080p desktop without requiring a
large vertical scroll. Controls that are not needed for an ordinary HMD test
are grouped and visually de-emphasized rather than shown beside the primary
launch flow.

## HMD readiness behavior

The readiness panel is local and deterministic. It validates the same
conditions already enforced by `Validate()` and adds explicit checks for the
packaged loader/module files and immersive projection settings. It must never
claim that a headset is connected unless the runtime can prove that through a
safe, bounded check; when that cannot be established, it reports “Ready to
launch — runtime connection is checked during startup.”

The public HMD preset is camera mode, stereo projection, motion input enabled,
performance profile enabled, body overlay disabled, and the current square XR
resolution policy left to VDXR/OpenXR. Existing saved settings remain
respected, while a new or legacy configuration receives these defaults.

## Behavior and safety

- Preserve the current `.ini` keys and migrate only presentation/default
  metadata as needed.
- Keep `--self-test` headless and deterministic.
- Keep dry run non-launching and HMD-independent.
- Keep launch validation fail-closed and continue refusing ambiguous running
  Catalyst processes through `launch_preview.ps1`.
- Avoid changing renderer, OpenXR, injection, or game-input behavior except
  where the launcher must pass the already-supported HMD preset switches.
- Use native controls and owner-draw/theme helpers only where needed; all
  resources must be released on shutdown.

## Verification

- Build the launcher in Release with warnings-as-errors.
- Run `mecvr_launcher.exe --self-test` from the staged package.
- Run the full Release CTest suite.
- Run a dry-run validation against the supplied retail executable and staged
  package.
- Perform a bounded retail launch/attach smoke with the HMD preset and verify
  that the launcher reaches the existing M3B attach path without leaving a
  Catalyst process running.
- Package the grouped launcher milestone with a detailed changelog; publish a
  release only after all checks pass.
