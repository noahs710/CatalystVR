# Catalyst runtime intelligence

Date: 2026-09-26. Retail target: `MirrorsEdgeCatalyst.exe` 1.0.3.47248.

## GPU transport integration

The camera module now creates a three-slot shared-texture ring on Catalyst's
D3D11 device. The OpenXR worker verifies that Catalyst and the runtime-selected
device have the same adapter LUID, opens each NT handle through
`ID3D11Device1`, and uses a nonblocking keyed-mutex handoff. A fullscreen GPU
blit center-crops the source independently to each enumerated eye texture's
actual dimensions. The Present path performs no readback while the handshake
is active. CPU capture remains a fail-closed fallback. This improves transport
cost but does not itself prove or implement the still-required native M6
same-prepared-scene dual render.

## Latest animation-seam evidence

The opt-in `MECVR_DISCOVER_PALETTES=1` pass now records shader-resource
bindings, vertex/index buffers, and draw signatures without modifying D3D11
state. A menu-state retail run produced repeated 1,056-, 1,392-, 1,856-,
2,000-, 2,848-, and 8,064-byte resources. Offline inspection shows the
1,056/1,392/1,856-byte captures contain screen/world-coordinate vertex-like
records rather than affine joint matrices; the 8,064-byte capture is a LUT-like
array. No verified character bone palette or stable animation consumer has been
identified, so GPU/retail pose writes remain disabled.

The read-only binary pass confirmed Frostbite metadata anchors including
`CameraRigFeature`, `CameraViewBinding`, `CameraView`, `CameraComponent`,
`ClientUpdateRenderer`, `ClientUpdateGame`, `ControllerUpdate`,
`SyncInputSceneOp`, `InputDeviceSceneOp`, and `ViewMatrixAsset`. These names
confirm that camera and input ownership are engine-level systems rather than
DXGI presentation artifacts.

The current D3D11 evidence still proves only the render-side CB seam: the
probe observes constant-buffer uploads, render targets, viewports, and
initialization-level context activity, but the retail smoke path reports no
draw or command-list execution callbacks. D3D11.1/2/3/4 device-context
interfaces and deferred-context creation are now covered across bounded vtable
classes. A fresh no-HMD smoke with that expanded coverage
(`%TEMP%\\mecvr_m3a_37400.log`) still recorded `draws_last=0`, `draw_idx=0`,
`execl=0`, `d12draws=0`, and `palettes=0`; the additional interfaces therefore
did not close the renderer-path gap.
The retail process loaded d3d11/dxgi but not d3d12 during the bounded smoke,
so the D3D12 observer remains dormant rather than being treated as the active
renderer. This does not yet identify a callable scene-render entry that can
be invoked twice without advancing simulation or duplicating side effects.
The stereo implementation therefore remains fail-closed: it does not replay
arbitrary command lists or issue a second Present. With `MECVR_ENABLE_STEREO=1`
it now uses the proven camera constant-buffer seam as a temporal producer:
consecutive game presents are rendered with the left and right located eye
poses, captured as a bounded pair, and submitted through the OpenXR projection
layer. This is a real per-eye image path, but it is temporally serialized and
must be validated on a headset before being promoted from experimental mode.

The M3B runtime path now has one XR worker/session that publishes the HMD
pose, captures the final D3D11 backbuffer into the bounded frame mailbox, and
submits that frame through the existing XR worker. This closes the camera-plus-
mono transport integration seam without claiming that it is yet a native
dual-pass game render.

The remaining engine-level gate is a repeatable `prepared scene -> eye render`
boundary for a simultaneous native dual-pass path. Until that boundary is
observed and validated, temporal stereo is the immersive default and mono
projection/quad remain explicit fallback paths.

## Static character package index

`tools/index_catalyst_assets.ps1` performs a read-only scan of the installed
Frostbite TOC/SB metadata. It records Data/Patch layer, archive path, package
category, and normalized package name; it never writes to the game directory.
The latest retail scan covered 48 archives and produced 3,619 de-duplicated
records: 3,231 character-package records, 296 animation records, 42
skeleton/IK records, and 50 runtime-type evidence records. It includes
`characters/ainpc/ainpc`, `characters/ainpc/nomad_locobindings`, customization
packages, separate head/lowerbody/upperbody mesh packages, and animation
state packages. This gives the future Catalyst-owned adapter a stable
asset/package vocabulary and Data-over-Patch provenance, while remaining
metadata-only evidence rather than a native runtime binding.

The executable also contains direct Frostbite type names for
`SkeletonAsset`, `MasterSkeletonAsset`, `SkinnedMeshAsset`,
`JointMappingFeature`, `TwoBoneIKFeature`, `TwoBoneIKController`,
`PoseToGlobal`, and `ApplyPoseNode`. These are strong static indicators of the
native animation and IK systems, but are not by themselves a safe runtime ABI.

A startup-attached play-scene capture was also run after manual menu
advancement. It observed a renderer resize to 1280x720 and additional
character-package-like SRV resources, but still reported zero D3D11 draw and
command-list callbacks. The live body consumer therefore remains disabled;
the mod does not guess Frostbite object offsets or write unverified GPU state.

A second controlled no-HMD smoke (`%TEMP%\\mecvr_m3a_35140.log`) used the
expanded context-interface hooks and automated menu input. It reached 6,832
Presents, recorded substantial constant-buffer/shader/map/unmap activity and
1280x720 scene targets, but still reported `draws_last=0`, `draw_idx=0`,
`execl=0`, `d12draws=0`, and `palettes=0`. This strengthens the conclusion
that the missing seam is not simply an unvisited menu state; the native
Catalyst body/skeleton consumer remains intentionally fail-closed.
