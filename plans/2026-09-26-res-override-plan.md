# VR Runtime Res/Refresh Override — Implementation Plan

- Spec: [2026-09-26-res-override-design.md](2026-09-26-res-override-design.md) (APPROVED)
- Status: PLANNED — no code until the M3a round-2 capture + review gate (D3).
- Gates: M2 transport mechanism frozen (hook *policy* only, zero vtable-mechanism
  changes); M3a stays observe-only (override OFF for all intel captures).

## 0. Grounding (verified in-tree)

- Swapchain + factory hooks are duplicated per probe: `src/render/m2b_live.cc`
  (≈148 swapchain, ≈356 factory) and `src/render/m3a_probe.cc` (≈952
  swapchain, ≈1304 factory). There is no shared hook layer, so the plan adds a
  shared *policy* module both call. Hook mechanism is untouched.
- `mecvr_render` STATIC = `vtable_hook.cc` + `observer.cc` (unit-test link).
- Probes are self-contained /MT DLLs; new shared sources must be listed in each
  probe target (`mecvr_m2b_live`, `mecvr_m3a_probe`) AND in `mecvr_render`.
- Only `mecvr_m2b_live` links `openxr_loader.lib`; `mecvr_m3a_probe` does not
  (affects WI-2: the runtime query must not fork behavior between builds).
- Tests wire via `add_executable` + `target_link_libraries` + `add_test` in
  `tests/CMakeLists.txt` (13 tests today; this plan adds a 14th).

## WI-1: Shared policy module (pure, testable)

- Files: `src/render/force_mode.h`, `src/render/force_mode.cc`.
- Contents:
  - `ResolveForcedMode(env_res, env_hz, runtime)`: precedence explicit-env >
    runtime-query > none. Parses `WxH` / `0` (kill-switch) / empty; validates
    and clamps (W/H 640..16384, Hz 60..240); invalid input = fail-closed
    (no override, never a guessed mode).
  - Pure struct in/out; no DXGI/OpenXR calls (unit-testable without devices).
- Wire `render/force_mode.cc` into `mecvr_render` in `src/CMakeLists.txt`.
- Done when: compiles; WI-3 test green.

## WI-2: Runtime query (fail-closed, uniform across probes)

- Same module (`force_mode.cc`): `QueryRuntimeMode()` opens a *transient*
  `XrInstance` (destroyed after the query), `xrGetSystem` →
  `xrEnumerateViewConfigurationViews(PRIMARY_STEREO)` → per-eye W/H from
  view[0] (log both; use max defensively if they differ).
- Refresh: enable `XR_FB_display_refresh_rate` if present and read the CURRENT
  rate. If the extension or query fails: dims-only override, desc `RefreshRate`
  preserved (fail-closed per-field, logged).
- Any failure at any step → `nullopt` → stock descs (spec §4 fail-closed).
- Build decision: link `openxr_loader.lib` into `mecvr_m3a_probe` exactly as
  `mecvr_m2b_live` does (include dirs + lib + POST_BUILD dll copy), so both
  builds behave identically. When the override is OFF (all M3a intel runs) no
  loader call is ever made. (Alternative if review objects: dynamic
  LoadLibrary; do not silently fork to env-only in one probe.)
- `MECVR_FORCE_HZ` overrides queried Hz; `MECVR_FORCE_RES=WxH` overrides all.
- Done when: query returns nullopt with no runtime present; returns sane
  values against VDXR (verified live in WI-5).

## WI-3: Unit test + suite wiring

- File: `tests/force_mode/force_mode_test_main.cc`, linked against
  `mecvr_render`; block in `tests/CMakeLists.txt` following the existing
  pattern; CTest name `force_mode_test` → suite becomes 14/14.
- Cases: valid `WxH`; `0` kill-switch; empty; malformed (no override);
  clamping bounds; Hz override/default; precedence env > runtime > none;
  runtime-nullopt → no override.
- Also add the row to `docs/TEST_MATRIX.md`.
- Done when: full `ctest` green.

## WI-4: Hook call-site integration (policy only)

- `src/render/m2b_live.cc`: in the ResizeBuffers detour and the
  CreateSwapChain desc path, call `ResolveForcedMode` once per call and, when
  a mode resolves, overwrite ONLY `Width`/`Height` (+ `RefreshRate` when a Hz
  resolved). All other fields/flags pass through. Log forced `WxH@Hz` + source
  (env vs runtime values) on the status line, once per session + on change.
- Same two call sites in `src/render/m3a_probe.cc` (≈952/≈1304 regions).
- Add `render/force_mode.cc` to both SHARED probe targets in
  `src/CMakeLists.txt` (self-contained /MT pattern).
- M3a intel protocol: all observe-only captures run with `MECVR_FORCE_RES=0`.
- Done when: builds clean; no vtable-mechanism lines touched (reviewable diff).

## WI-5: Live validation (after M3a round-2 review gate)

1. Inject with override ON; log shows forced `WxH@Hz` + runtime source values.
2. Game renders the forced res (confirm via probe RT sizes / Present stats).
3. Mono panel shows the FULL forced frame, centered (M2 centering preserved).
4. fps sanity note at 2×eyeW×eyeH; if it tanks, user revisits D1 with data.
5. Re-run the M2 4 visual checks (source dims changed → transport re-confirm).
6. Kill-switch check: `MECVR_FORCE_RES=0` → stock descs, game at 3440×1440.
- Done when: 1–6 recorded in the validation log.

## WI-6: Rollback / contingency (no code unless triggered)

- Instant relief: `MECVR_FORCE_RES=0` (no rebuild).
- Approach-B trigger (spec §3): game ignores the forced desc twice, or the
  in-game settings UI visibly fights the override → new plan for output-mode
  spoofing. Do not improvise B inside this plan.

## Out of scope (restated from spec)

HMD-paced present scheduling (M5/M6); windowed/fullscreen flag forcing;
approaches B/C unless WI-6 triggers.

## Sequencing

1. Now: this plan only. 2. Next: M3a round-2 capture + review. 3. Then: WI-1 →
   WI-4 in order, WI-5 live, WI-6 only if triggered.
