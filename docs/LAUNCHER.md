# MECVR Launcher

`mecvr_launcher.exe` is the public Windows entry point for the preview
package. It keeps the retail executable path and runtime choices in
`%LOCALAPPDATA%\MECVR\launcher.ini`, then hands off to the packaged
`launch_preview.ps1` script.

The GUI exposes the opt-in 6DoF camera bridge, STRIDE-style motion/input,
experimental temporal stereo, turn mode, units-per-meter calibration,
projection/quad presentation mode, quad-space selection, and opt-in palette
diagnostics and the opt-in mod-owned IK body overlay. The body overlay is off
by default because it is a desktop-space diagnostic bridge, not native Catalyst
body rendering. Palette diagnostics are
development-only, disabled by default,
and never enable GPU or retail-memory writes. An optional solved-motion clip
recording path is also exposed; clips are capped at 1,800 frames and written
only when explicitly configured.
Camera mode defaults to full-eye projection with stereo enabled. The quad path
is an explicit diagnostics/cinema fallback and appears as a theatre panel by
design; it is not the immersive gameplay mode.
The raised-hands physical-jump gesture and calibrated physical-crouch gameplay
input are separately persisted. Either bridge can be disabled without
disabling the mod-owned body animation or controller input.
An additional opt-in parkour bridge maps vault to Space, climb to E, and slide
to crouch; it is disabled by default because these are desktop input routes.
The launcher exposes the three scan-code fields (defaults 57/18/29) so the
bridge can match a user's keyboard layout without rebuilding the mod.
The same control can select a previously saved clip for looped playback while
live head/camera tracking continues.
`Dry run` validates the selected executable and package without starting the
game and never requires an HMD or OpenXR runtime.

Runtime mode is explicit and persisted: `camera` selects the M3B 6DoF/capture
path, while `mono` selects the stable M2B desktop/quad path. The launcher
passes the selected mode directly to `launch_preview.ps1`.

The launch backend is also explicit and defaults to `direct`. The optional
`frosty` backend starts a configured Frosty pack, waits for the exact new
Catalyst executable, and then performs MECVR injection. It never edits
Frosty profiles, ModData, CAS, or game files.

When a motion clip path is configured, the XR worker serializes the solved
mod-owned IK frames on clean shutdown in a versioned binary format. This is
independent of Catalyst's private animation ABI and can be replayed by future
native skeleton adapters.

For automated package validation, run:

```text
mecvr_launcher.exe --self-test
```

The release launcher is deliberately thin: injection, OpenXR session setup,
fail-closed camera writes, capture, and frame submission remain in the
runtime components launched by `launch_preview.ps1`.
