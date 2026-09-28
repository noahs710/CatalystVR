# CatalystVR Launcher

`mecvr_launcher.exe` is the public Windows entry point for the preview
package. It keeps the retail executable path and runtime choices in
`%LOCALAPPDATA%\MECVR\launcher.ini`, validates the packaged payload, and
hands off to `launch_preview.ps1`.

## HMD-first workflow

1. Select `MirrorsEdgeCatalyst.exe` in the Target card.
2. Press `Quest 3 / VDXR preset`.
3. Confirm the HMD Readiness card shows four green checks.
4. Press `Launch VR` after VDXR/OpenXR is selected as the active runtime.

The readiness card checks the executable, package, OpenXR loader plus M3B
camera bridge, and the immersive profile. It does not pretend to detect a
connected headset in advance; the active OpenXR/VDXR runtime performs the
connection check during startup.

The preset selects camera mode, immersive stereo projection, tracked motion
input, performance pacing, and disables the diagnostic body overlay. VDXR and
the active OpenXR runtime retain ownership of requested headset resolution and
refresh rate. CatalystVR never forces a desktop 16:9 surface or overrides the
runtime's headset settings.

## Runtime controls

The main window keeps the high-frequency controls visible: camera bridge,
motion input, immersive stereo, turn mode, performance profile, and projection
mode. `Advanced Settings` contains diagnostics, physical jump/crouch, the
optional parkour presentation hints, Frosty launch routing, scan-code mapping,
motion clip capture/playback, and the fail-closed native Faith contract
controls.

The default `performance` profile keeps the shared D3D11 GPU transport active,
disables the debug body overlay, and avoids extra readbacks. AFR and other
frame-generation or pacing features remain owned by the active runtime and are
compatible with the launcher because CatalystVR does not replace the runtime's
requested resolution or refresh rate.

`Dry Run` validates the selected executable and package without starting the
game. It also reports whether the immersive HMD profile is active. It never
requires an HMD or OpenXR runtime.

## Input and fail-closed behavior

Quest Touch Plus grip closes each mod-owned hand into a fist. A gripped swing
produces a one-shot combat intent, and a gripped pull toward the body latches
the MAG-rope intent. Jump remains a held game action so Catalyst can own ledge
autograb/climb decisions. Left-stick locomotion remains authoritative with
tracked-hand swing fallback, and right-stick turning remains available.

The mod-owned IK layer is a deterministic procedural 21-joint pose path. It is
currently intended for arm/hand motion and diagnostics; native Faith skeleton
writes remain disabled unless a reviewed, geometry-matched live palette
contract is explicitly supplied.

## Automated validation

Run the packaged launcher self-test from the package directory:

```text
mecvr_launcher.exe --self-test
```

The launcher is deliberately thin. Injection, OpenXR session setup,
fail-closed camera writes, capture, and frame submission remain in the runtime
components launched by `launch_preview.ps1`.
