# CatalystVR changelog

## 1.5.2.10-alpha — 2026-09-28

### Stereo projection milestone

- Replaced the CPU eye upload letterbox path with the same centered square/near-square crop contract used by the GPU compositor. Every runtime-selected eye pixel is filled; no 16:9 desktop image, black bars, zoomed theatre panel, or stale staging contents are submitted.
- Added independent per-eye temporal capture throttles so the second eye cannot be dropped at high refresh rates while pairing consecutive game presents.
- Added an end-to-end worker seam test that records both eye uploads, proves independent payloads reach both projection swapchains, proves the mono path is bypassed, and rejects stale cross-tick pairs.
- Added mock-backend stereo upload instrumentation so the projection contract is continuously testable without an HMD.
- Kept runtime-owned eye dimensions and refresh pacing intact; no launcher override changes those OpenXR runtime choices.

### Verification

- Release build: passed.
- Full CTest matrix: 36/36 passed.
- Focused stereo, epoch, and crop tests: passed.

## Unreleased

### Native Faith contract capture and square XR transport

- Added a one-shot M3B capture path that waits for a stable, complete,
  geometry-matched Faith arm palette and writes a reviewable native bone
  contract artifact.
- Kept the capture path fail-closed: it never auto-loads the artifact and never
  enables native palette writes.
- Added launcher and `launch_preview.ps1` controls for capture output paths,
  persisted settings, and automatic diagnostic palette discovery.
- Made the camera transport crop CPU eye frames to square surfaces before XR
  submission so desktop 16:9 framing cannot leak into immersive presentation.
- Bumped launcher settings migration to version 4 and expanded deterministic
  launcher coverage for the capture contract.

## Unreleased — 1.5.2.8-alpha

### Deterministic Faith arm contract capture

- Added a geometry-gated builder that converts a complete, identified Faith-shaped palette observation into the eight known arm mappings only after executable, resource-size, layout, and bilateral-geometry checks pass.
- Added reviewable contract serialization using the existing strict native-bone-map format; omitted torso and leg entries remain explicit partial-map behavior.
- Added positive synthetic proof, asymmetric-palette rejection, truncated-resource rejection, and round-trip contract tests.
- Kept retail writes fail-closed: ambiguous runtime buffers still cannot arm the native writer, and the capture design/spec records the evidence required before enabling a title-specific map.

## Unreleased — 1.5.2.7-alpha

### Staged Faith palette identity

- Added bounded D3D11 `CopyResource` and full-region `CopySubresourceRegion` tracking so staged Faith palettes retain their verified source-to-bound-resource identity.
- Added direct vertex-binding evidence from `VSSetShaderResources`, allowing the target tracker to verify a stable palette even when the title records or submits draws through an unobserved path.
- Native arm writes can now target the bound SRV resource while rewriting only mapped upload bytes, subject to the existing executable, layout, pose-freshness, and native bone-map gates.
- Kept copy propagation discovery-only unless the explicit native contract is loaded; ordinary performance launches remain unaffected.

## Unreleased — 1.5.2.6-alpha

### Stereo transport and launcher clarity

- Give explicit stereo priority over the mono shared-GPU fast path so the two captured eyes cannot be silently replaced by one image.
- Preserve the complete Catalyst source image when uploading to each runtime eye texture instead of center-cropping into a zoomed view.
- Make temporal stereo mandatory whenever stereo is enabled, including performance-mode launches, so eye pairs are actually produced.
- Reworked the launcher into a wider, non-overlapping layout with grouped runtime, backend, motion, and native-contract sections.
- Render temporal stereo through a centered square viewport and transport a square eye frame, keeping the desktop swapchain as an internal source only.

## Unreleased — 1.5.2.4-alpha

### Live palette diagnostics

- Diagnostic palette discovery now records bounded content-fingerprint
  transitions per SRV-backed resource, distinguishing animated buffers from
  static lookup data before any native IK contract is authored.

## Unreleased — 1.5.2.3-alpha

### Faith palette geometry gate

- Added a runtime matcher for the verified retail Faith arm chain at indices
  `8, 9, 12, 15, 111, 112, 115, 118`.
- Native palette observation and native pose writes now require all eight arm
  matrices, bilateral segment symmetry, and one consistent retail scale.
- Added model-space and local-space matching with resource reset on mismatch, so
  an unrelated animated buffer cannot inherit a previously verified write
  target.
- Added `faith_palette_match_test` and the retail geometry-binding research
  note.

## Unreleased — 1.5.2.2-alpha

- Added a read-only Catalyst SkeletonAsset extraction path and a narrow
  self-describing EBX parser independent of Frosty's generated managed ABI.
- Verified the retail female skeleton's 169-bone hierarchy, bind/model poses,
  source SHA-256, and native arm chains at indices `8/9/12/15` and
  `111/112/115/118`.
- Added a compiled Faith arm identity/rest-pose contract with hierarchy and
  mirrored-length tests. The indices do not bypass existing executable,
  resource, layout, freshness, or bounds gates.

## Unreleased — 1.5.2.1-alpha

- Replaced the mod-owned arm fallback's hand-tuned screen-space projection
  with the current tracked OpenXR eye pose and runtime asymmetric frustum.
  Controller motion now retains horizontal, vertical, and depth translation
  through IK and visible rendering in the same coordinate system as the scene.
- Removed the fixed-IPD overlay approximation. Each temporal eye now projects
  arms, hands, fingers, and weapon geometry from its actual located pose.
- Added regression proof that raising a tracked arm changes visible vertical
  geometry and that distinct runtime eye poses produce distinct stereo output.

## Unreleased — 1.5.2.0-alpha

- Replaced Catalyst's desktop projection at the confirmed scene-CB byte 96
  with each OpenXR eye's runtime-provided asymmetric frustum. The guarded
  rewrite preserves Catalyst's 0.06-unit near plane and infinite-far depth
  convention, while malformed or unexpected buffers remain untouched.
- Made eye view and projection rewrites atomic so a frame can never receive
  only half of the stereo camera transform.
- Kept swapchain dimensions and refresh pacing entirely runtime-owned; no
  VDXR resolution or refresh override is introduced.
- Added regression coverage for asymmetric eye projection, near-plane
  preservation, and fail-closed projection classification.

## Unreleased — 1.5.1.25-alpha

- Added guarded vertex-stage SRV binding evidence for Catalyst paths that
  expose stable skinning palette binds without observable Draw or
  ExecuteCommandList callbacks. Native promotion still requires a classified
  stable resource, non-null vertex shader, multi-present freshness, and the
  existing executable/map/layout/bounds gates.

## Unreleased — 1.5.1.24-alpha

- Fixed native palette-to-draw correlation for deferred D3D11 contexts by
  retaining bounded VS/PS SRV and shader state per context. Draw promotion no
  longer depends on immediate-context globals, improving the path that can
  safely identify Faith's live skinning resource.

- Added a guarded `UpdateSubresource` native-pose rewrite path for structured
  SRV buffers, covering Catalyst skinning uploads that bypass the existing
  Map/Unmap probe. It copies into a bounded scratch buffer and only forwards
  solved arm matrices after the same verified resource/layout/freshness checks.
  Unknown or unverified uploads remain byte-for-byte untouched.

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
