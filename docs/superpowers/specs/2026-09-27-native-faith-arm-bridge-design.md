# Native Faith Arm Bridge Design

**Date:** 2026-09-27

**Status:** Approved for autonomous implementation as the next bounded alpha slice

## Problem

The mod already has a canonical motion pose, an OpenXR mailbox, a Faith skeleton contract, and a guarded D3D11 palette path. The missing piece is a reproducible bridge from an observed game resource to a verified Faith arm palette target. The current runtime correctly refuses to write when that bridge is not proven, but an alpha tester cannot produce a safe `NativeBoneMap` from the data it observes.

Resource dumps seen during autonomous retail smoke are ambiguous: several buffers contain coordinate-like data, while the 8064-byte candidate does not exhibit Faith's known arm geometry when interpreted as affine matrices. Creating a map from those dumps would risk corrupting unrelated game resources or silently deforming Faith.

## Goal

Turn the existing observe-only path into a contract-driven workflow that can:

1. classify candidate source resources;
2. correlate source updates, GPU copies, SRV bindings, and draw-time identity;
3. prove that a candidate contains the expected Faith arm palette layout;
4. emit a versioned native contract only after all identity and geometry checks pass; and
5. permit runtime writes only when the contract, executable fingerprint, resource identity, pose freshness, and bounds checks all agree.

This slice does not claim a live writer until the retail executable produces the evidence required by the contract. A failed capture remains a successful safety outcome.

## Non-goals

- Guessing a bone stride, matrix orientation, or resource offset from resource size alone.
- Replacing the game's whole skeleton or animation graph in this slice.
- Adding a shader replacement path that bypasses the native palette contract.
- Treating desktop or theatre-mode presentation as proof of XR stereo correctness.

## Alternatives

### Native D3D11 source/copy rewrite — selected

Capture the actual palette resource used by Faith, prove its layout, and rewrite only the known arm entries before the game consumes them. This preserves the game's skinning, materials, culling, and animation scheduling while keeping the write surface small and auditable.

### Vertex-shader replacement

Replace the skinning shader and provide a parallel pose buffer. This could work without finding the original palette, but it expands shader compatibility risk, makes material permutations harder to support, and is more likely to break with driver or game updates.

### External overlay / composited arm model

Render tracked arms as an overlay. This is useful as a diagnostic fallback but cannot provide native occlusion, lighting, shadows, contact, or Faith's existing skinning behavior.

## Runtime data flow

```text
OpenXR BodyPoseMailbox
        │
        ▼
CanonicalPose + freshness sequence
        │
        ▼
Faith arm contract matcher ────────┐
        │                          │
        │ proven                    │ rejected / stale
        ▼                          ▼
NativePoseMatrices          diagnostics only
        │
        ▼
D3D11 source update / copy / SRV binding correlation
        │
        ▼
Verified NativeBoneMap (versioned, fingerprinted)
        │
        ▼
Bounded native arm-palette rewrite
```

The capture side must retain source resource identity, destination/copy aliases, byte size, bind flags, update sequence, copy sequence, observed SRV binding, draw/present age, and the candidate's layout evidence. The writer must never infer missing fields at runtime.

## Contract shape

The contract artifact is versioned and includes:

- game executable fingerprint and contract schema version;
- skeleton asset identifier and skeleton hash;
- exact source and, when applicable, destination resource identity;
- resource byte size and required bind/usage flags;
- matrix encoding (`affine3x4`, `4x4`, or another explicitly implemented encoding);
- byte stride, base offset, matrix count, and the Faith bone-to-entry mapping;
- row/column-major and coordinate-space declaration;
- bilateral arm geometry evidence and tolerances;
- capture sequence metadata and a human-readable provenance note.

Unknown or unsupported fields make the artifact invalid. The runtime loader rejects an artifact with a mismatched executable fingerprint, skeleton hash, schema, size, stride, offset, matrix encoding, or mapping bounds.

## Proof gates

Contract generation and runtime writes are both fail-closed. All of these gates must pass:

1. The executable fingerprint matches the contract.
2. The resource identity is exact, or is a directly observed copy alias with a bounded age.
3. The resource size and bind flags match.
4. The candidate has a supported matrix layout and sufficient byte bounds.
5. Faith's bilateral arm entries match the known skeleton geometry within tolerance.
6. The same target is observed at a stable SRV vertex binding over multiple frames.
7. The body pose sequence is fresh and monotonic.
8. Every rewritten entry is within the contract's declared bounds.
9. Any failed observation resets the armed writer state and produces diagnostics without modifying the resource.

The matcher must expose rejection reasons and counters so an alpha tester can distinguish “no render draw observed” from “candidate layout rejected” and from “contract accepted but pose stale.”

## Testing strategy

### Deterministic synthetic harness

Build a test-only resource containing a known affine palette and a known Faith arm mapping. Exercise source update, copy alias, SRV binding, pose freshness, and bounded rewrite in several frame orders. Verify that:

- the correct destination is selected after a copy;
- stale aliases cannot arm the writer;
- invalid offsets, stride, matrix count, and executable fingerprints are rejected;
- both arms are rewritten while unrelated entries remain byte-identical;
- a stale pose produces no write;
- a failed Faith match resets the armed state.

### Retail observation

Run the packaged launcher against the installed executable with diagnostics enabled. Record the executable fingerprint, present count, draw/binding counts, candidate rejection reasons, and whether an HMD is available. Do not describe the native writer as active unless a verified contract is loaded and at least one bounded write is observed.

### Performance

Keep capture logging bounded and disabled outside diagnostics. The steady-state writer should touch only the declared arm entries and avoid per-frame allocations. The XR presentation path remains square/square-like and must not inherit the desktop 16:9 swap-chain dimensions.

## Rollout

The first implementation adds deterministic contract validation/capture and synthetic proof coverage. Live runtime writes remain behind the existing contract and Faith-match gates. Once retail evidence proves a contract for the exact executable build, that artifact can be added to a future grouped alpha release with a detailed changelog.

## Self-review

- No unsafe fallback is introduced.
- Ambiguous dumps are rejected rather than promoted to maps.
- The design preserves the current native-write safety boundary.
- The test plan covers both positive and negative identity/layout cases.
- The artifact is useful even when retail capture fails because rejection telemetry makes the next capture actionable.
