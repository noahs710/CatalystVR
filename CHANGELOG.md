# CatalystVR changelog

## Unreleased — 1.5.1.1-alpha

- Added a persisted launcher control for preserving runtime-owned frame pacing,
  AFR, and compositor frame generation.
- Added `-PreserveRuntimePacing` to the PowerShell launch path and exported the
  explicit `MECVR_PRESERVE_RUNTIME_PACING` policy to injected modules.
- Kept MECVR presentation work free of synthetic presents and local frame
  waits so VDXR, SteamVR, Meta OpenXR, AFR, and runtime frame generation remain
  authoritative.
- Verified the Release build and all 35 automated tests.
- Ran an observe-only retail probe against the supplied executable; the
  menu-phase run was stable but produced no Faith skeleton palette evidence,
  so native skeleton writes remain fail-closed.

The next public release will include this launcher/performance group together
with the next verified runtime or IK milestone and will carry a detailed
release-specific entry here.
