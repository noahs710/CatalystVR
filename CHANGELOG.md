# CatalystVR changelog

## Unreleased — 1.5.1.22-alpha

- Added read-only `UpdateSubresource` observation for structured SRV buffers,
  covering Catalyst skinning uploads that bypass the existing Map/Unmap probe.
  The source pointer is never rewritten on this path.

- Native Faith-arm writer now supports verified eight-joint arm maps without
  requiring torso or leg indices; unmapped regions remain game-animated.
- Supplying `MECVR_NATIVE_BONE_MAP` now automatically enables bounded palette
  capture/correlation, while all executable/resource/layout/freshness guards
  remain fail-closed before any native write.

- Enabled the tested mod-owned IK arms/body overlay by default in both GUI and
  script launchers; `-DisableBodyOverlay` remains the explicit performance
  fallback.

- Fixed explicit `-DiscoverPalettes` capture mode being overridden by the
  launcher's default performance profile; requested contract discovery now
  automatically selects diagnostic performance settings.

- Expanded unattended M3B transport telemetry with stereo submitted/rejected
  counts, upload latency, present/XR rates, and frame age.

- Launcher now enables the guarded temporal dual-eye producer by default so
  public alpha launches request immersive stereo instead of mono projection.
- `-DisableStereo` remains available for performance comparison and mono
  fallback diagnostics.

- Removed unconditional 64 KiB palette-content hashing from the read-only
  render path. Fingerprinting now runs only when an explicit native bone map
  is loaded, reducing unnecessary CPU/memory traffic during normal alpha use.

- Added retail archive asset-index evidence for Faith and shared Catalyst
  skeleton packages, including Data/Patch layer coverage and the verified
  executable fingerprint.
- Kept the evidence explicitly separate from the guarded native bone writer;
  package names do not authorize a runtime palette or joint map.

- Added `fingerprint_executable.ps1`, an offline utility that emits the exact
  executable fingerprint format required by reviewed native contracts without
  fabricating resource or joint mappings.

- Fixed the native Faith bone-contract executable gate so it fingerprints the
  running executable on disk instead of trusting the contract's own claimed
  fingerprint. Hash failure or mismatch now keeps native writes disabled.

- Extended the read-only compatibility scanner to flag common Catalyst
  Frosty/DataPathFix plugin modules as advisory asset/launch conflicts.

- Added a cited research note covering existing Catalyst camera, animation,
  Frostbite asset, and VR modding work, with reuse and licensing boundaries.

- Extended D3D11 native-palette draw correlation to instanced and indirect
  draw families, so skinned meshes are not discarded merely because they do
  not use the two basic draw entry points.

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
- Hardened stereo submission validation to reject unequal left/right image
  dimensions before swapchain upload, preventing geometric eye misalignment.
- Added bounded per-context D3D11 draw/finish/execute telemetry to distinguish
  missing scene draw hooks from command-list execution gaps during autonomous
  retail investigation.
- Recorded the follow-up retail result: resource updates and presents occur,
  but no D3D11/D3D12 draw or command-list execution reaches the current hook
  surface in the observed phase, so palette candidates remain non-semantic.
- Added early launcher and PowerShell validation for an explicitly configured
  native bone-contract path; the ordinary blank-path configuration remains
  valid and fail-closed.
- Verified the Release build and all 35 automated tests.
- Ran an observe-only retail probe against the supplied executable; the
  menu-phase run was stable but produced no Faith skeleton palette evidence,
  so native skeleton writes remain fail-closed.

The next public release will include this launcher/performance group together
with the next verified runtime or IK milestone and will carry a detailed
release-specific entry here.
