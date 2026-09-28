# Native Faith Contract Capture Workflow

**Date:** 2026-09-27

**Status:** Approved for autonomous implementation as the next alpha slice

## Objective

Give an alpha tester a deterministic, HMD-independent way to capture the
reviewable native Faith arm contract from a live Catalyst process. The capture
must reduce the current gap between “the probe observed a candidate” and “a
reviewed `MECVR_NATIVE_BONE_MAP` exists,” while preserving the existing
fail-closed native writer.

## Scope

The workflow adds:

- a launcher/script diagnostic toggle and optional output path;
- runtime capture telemetry and a one-shot capture latch;
- a capture gate requiring a complete resource, Faith bilateral geometry,
  stable source observations, and stable vertex-bound target evidence;
- reuse of `BuildFaithArmNativeBoneMap` and `WriteNativeBoneMap` to emit the
  strict versioned text contract; and
- headless tests for argument propagation, default behavior, and the existing
  native-map proof.

The runtime writes the artifact only. It does not load the new artifact into
the current process, enable native writes, alter game memory, or require an
HMD. A later launch must explicitly select the reviewed file.

## Data flow

```text
Launcher checkbox/path
        │
        ▼
launch_preview.ps1 environment
        │
        ▼
M3B one-shot capture request
        │
        ├─ complete mapped source bytes
        ├─ Faith arm geometry match
        ├─ stable NativeSkeletonAdapter source
        └─ stable NativePaletteTargetTracker vertex binding
        │
        ▼
BuildFaithArmNativeBoneMap → WriteNativeBoneMap
        │
        ▼
reviewable .map + bounded log evidence
```

## Capture gates

The request remains pending until all gates pass:

1. Palette discovery is enabled explicitly by the capture request.
2. The mapped source is complete and no larger than the diagnostic scan limit.
3. The classifier returns a supported candidate and Faith's eight arm entries
   match with bilateral topology and consistent scale.
4. The source adapter has promoted the same resource/layout across its stable
   observation window.
5. The target tracker has promoted repeatable vertex-binding evidence for the
   resource or a directly observed copy alias.
6. The executable fingerprint and resource size are non-zero.
7. Artifact generation and serialization both succeed.

Any failed condition emits a bounded reason and leaves the request armed for a
future frame. A successful write clears the request, logs the path and map
metadata, and leaves the native map loader disabled until the next launch.

## Interface

PowerShell exposes:

- `-CaptureNativeBoneMap` — enable one-shot capture;
- `-NativeBoneMapCapturePath` — optional absolute output path. If omitted, the
  runtime uses a per-process file under `%TEMP%`.

The launcher persists the toggle and path beside the existing optional native
map field. The UI labels it as diagnostic capture and explains that it does
not enable native writes.

Runtime environment variables are private implementation details:

- `MECVR_CAPTURE_NATIVE_BONE_MAP=1`;
- `MECVR_CAPTURE_NATIVE_BONE_MAP_PATH=<path>`.

## Safety and performance

Capture is disabled by default and forces diagnostic palette discovery only
when explicitly requested. It performs no per-frame file I/O: the file is
written once after all gates pass. The output path is bounded and logged; the
existing normal performance profile remains free of palette hashing and
capture work.

## Verification

- launcher self-test checks the packaged script and binaries;
- PowerShell tests verify capture arguments and environment propagation;
- native-map tests prove capture accepts the synthetic Faith palette and
  rejects asymmetric or truncated data;
- retail smoke verifies the new request/path status lines and confirms that a
  capture request with no Faith match never creates an artifact or arms the
  writer.

## Alternatives considered

### Manual map authoring

Rejected as the default workflow because it encourages guessing offsets and
strides from ambiguous dumps.

### Automatic write arming after capture

Rejected for safety and reviewability. A captured artifact must be inspected
and explicitly selected on a later launch.

### External post-processing only

Rejected as the primary path because it cannot verify live SRV binding and
copy identity; it remains useful for forensic analysis.

## Self-review

- The capture path is opt-in, one-shot, and HMD-independent.
- It cannot promote a candidate without both source stability and bound-target
  evidence.
- It does not weaken executable, layout, freshness, or bounds checks.
- It gives testers a concrete artifact and telemetry rather than asking them
  to interpret raw binary dumps.
