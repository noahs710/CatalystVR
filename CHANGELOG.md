# CatalystVR changelog

## Unreleased — 1.5.1.5-alpha

- Added a persisted launcher control for preserving runtime-owned frame pacing,
  AFR, and compositor frame generation.
- Added `-PreserveRuntimePacing` to the PowerShell launch path and exported the
  explicit `MECVR_PRESERVE_RUNTIME_PACING` policy to injected modules.
- Kept MECVR presentation work free of synthetic presents and local frame
  waits so VDXR, SteamVR, Meta OpenXR, AFR, and runtime frame generation remain
  authoritative.
- Connected the guarded M3B non-constant-buffer palette seam to the solved IK
  pose-matrix builder. The seam records native-pose write attempts and
  rejection reasons, but remains fail-closed until a validated title-specific
  skeleton map and executable/layout contract are available.
- Hardened XR pose publication for startup, focus loss, and runtimes that
  temporarily report fewer than two views. Head position now falls back to the
  available view, while stereo state is only published when both eyes exist;
  this prevents invalid eye indexing from dropping the entire motion frame.
- Added a strict, opt-in native bone-map contract loader. `MECVR_NATIVE_BONE_MAP`
  can point to a versioned text contract; malformed or incomplete contracts
  remain rejected by the existing executable/resource/layout/uniqueness gates,
  while the default path stays fully fail-closed.
- Exposed the native contract path in the persisted launcher settings and
  launch handoff, so verified title-specific adapters no longer require manual
  environment editing.
- Added a reproducible observe-only retail validation record documenting the
  stable 193 FPS probe run, palette-shaped resources, and the evidence still
  missing before native Faith writes can be enabled.
- Fixed explicit temporal stereo capture being bypassed by the successful
  shared-GPU mono path. Stereo mode now pairs separate left/right captures;
  normal performance mode continues to prefer shared-GPU transport.
- Made explicit stereo take precedence over the default runtime-pacing
  preservation checkbox, preventing the launcher’s AFR-safe default from
  silently disabling the requested per-eye producer.
- Verified the Release build and all 35 automated tests.
- Ran an observe-only retail probe against the supplied executable; the
  menu-phase run was stable but produced no Faith skeleton palette evidence,
  so native skeleton writes remain fail-closed.

The next public release will include this launcher/performance group together
with the next verified runtime or IK milestone and will carry a detailed
release-specific entry here.
