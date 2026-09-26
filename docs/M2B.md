# MECVR M2B Mono Submit (T11, render stream) + T10G Gate Record

Status: `M2 SOFTWARE GATES COMPLETE — LIVE XR TRANSPORT PROOF PENDING`
(see docs/M2_RECORD.md). No M3 camera work begins before the live proof.

## Architecture (plan Key Decisions 2–4, unchanged)
- XR worker owns the entire XR frame loop (`xrWaitFrame`, begin, locate,
  acquire/release, end) via `IXrBackend`; see `src/openxr/xr_frame_worker.*`.
- Game Present thread owns game immediate-context work only; the sole
  channel is the bounded `FrameMailbox` (capacity 2, short mutex, never
  waits on XR); see `src/openxr/mailbox.h`.
- Newest safe completed frame wins; older frames supersede (counted);
  idle ticks re-show the last frame; shutdown drains boundedly.
- Identical mono pixels to both eyes (`same_storage`, byte-verified).
- No camera/stereo/gameplay/timing code anywhere in M2B modules —
  attested by `M2bCameraUntouched()` + `static_assert`, asserted in-test.
- Diagnostic eye log is a capped ring (256); totals live in counters
  (fixed during T11 integration: unbounded growth would leak).

## Instrumentation (per plan, all in M2bStats)
Present rate, XR rate, predicted interval, capture-to-submit age
(last/max/mean), reuse count, superseded/dropped counts, mailbox
depth/high-water, copy/acquire/release/wait timings, missed/late frames,
shutdown drain. Non-negative frame age is a LIVE-gate assertion: headless
MockXR uses a deterministic virtual clock, so the headless suite asserts
boundedness; see `tests/m2b/m2b_mono_test_main.cc` comment.

## Headless validation (all green in ctest)
- Transport: 200/90 e2e (identical eyes every submit, newest-wins,
  balanced accounting, bounded depth, reuse, drain, S3 attestation).
- Matrix: 200/90, 60/90, 90/90, 45/90 + stall/resume + shutdown-pending +
  60k-publish soak (exact balance, bounded, capped diagnostics).
- T10G gate tool verdicts: PASS (match) / FAIL (mismatch) / OPEN
  (headset absent, exit 2, never faked).

## T10G gate record (seven fields)
- Catalyst adapter: `AMD Radeon RX 9060 XT`, vendor `0x1002`,
  device `0x7590`, LUID `0x00000000:0x0001a699`, FL 11.1
  (OBSERVER_M0C2.md, retail 1.0.3.47248).
- OpenXR-required LUID / minimum feature level: UNKNOWABLE headless —
  `xrGetSystem` → `XR_ERROR_FORM_FACTOR_UNAVAILABLE` against
  VirtualDesktopXR v1.0.10 (no headset). Recorded adapter confirmed
  PRESENT in local DXGI enumeration.
- Verdict: OPEN. Mismatch behavior (both sides logged + plain-language
  note, integrated path refused) is implemented but untriggered.
- Gate stays MANDATORY for the integrated real path; MockXR bypass is
  test-only.

## Regression baselines preserved for M3
T3B: 5.47 us in-hook overhead, `state_fail=0` across ~18k Presents,
~5.0 ms cadence. T10: 0/9930 backbuffer mismatches. Any M3 camera/stereo
regression is attributable against these numbers (HOOK_EVIDENCE.md).
