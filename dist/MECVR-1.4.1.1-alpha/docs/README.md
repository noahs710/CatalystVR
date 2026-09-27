# MECVR

MECVR is an in-development OpenXR conversion layer for Mirror's Edge
Catalyst. The current public preview contains the tested runtime foundation,
opt-in camera bridge, motion-scheme library, and guarded stereo submission
seam.

This is not yet a complete playable VR conversion. A headset is required for
live XR proof, and the game-specific dual-pass renderer, HUD conversion,
comfort UI, and final input injection remain active milestones.

Build a Release configuration, run `ctest --test-dir build -C Release`, then
run `tools/package_release.ps1` to produce a self-describing bundle under
`dist/`. The package refuses to complete when tests fail.

The preview bundle includes `launch_preview.ps1`, which starts the selected
game executable, injects the OpenXR loader first, then attaches the MECVR
module. Camera mode is opt-in at runtime and still requires an OpenXR headset
and a validated `MECVR_UNITS_PER_METER` calibration. Passing `-EnableInput`
enables the foreground-gated keyboard/mouse bridge for the STRIDE-style
movement scheme; it is off by default.
Use `-TurnMode snap` for 45-degree comfort turns with hysteresis; smooth is
the default. The launcher defaults to immersive projection with the temporal
stereo producer enabled. `-DisableStereo` returns to mono projection, while
selecting `quad` explicitly enables the theatre/cinema fallback. Temporally
serialized left/right rendering remains an interim path until the simultaneous
native dual-pass seam is proven.
