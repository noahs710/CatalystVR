# VR Runtime Res/Refresh Override — Design

- Status: APPROVED (2026-09-26)
- Relates to: spec M13 (Automated Resolution and Refresh Management); pulls the
  runtime-driven portion of M13 forward. Independent of M3a camera intel.
- Hard gates respected: M2 transport frozen (this changes hook *policy*, not the
  M2 quad path); M3a stays observe-only — this ships OFF during round-2 intel.

## 1. Intent

Run Mirror's Edge Catalyst at the VR runtime's requested resolution and refresh
rate — overriding the game's resolution, aspect ratio, and refresh — so the game
image is properly conditioned for VR display from the mono phase through M6
stereo. One policy now, no re-tuning at the M6 splice.

Success criteria:

1. Game backbuffer is exactly `2 × eyeW × eyeH @ runtimeHz` during VR sessions.
2. Game-internal aspect/projection follow the forced backbuffer (verified live).
3. Mono panel shows the full forced frame, centered (M2 centering preserved).
4. Kill-switch restores stock behavior with zero code change.
5. Zero impact on M3a intel: override is OFF for all observe-only captures.

## 2. User-approved decisions

- D1 (mono res): full stereo width now — `W = 2 × per-eye-W, H = per-eye-H`.
- D2 (mono presentation): full frame letterboxed on the centered quad.
- D3 (sequencing): implement only after the M3a round-2 capture + review gate.

## 3. Approaches considered

- A) Swapchain-desc rewrite (CHOSEN): overwrite width/height/refresh in the
  existing CreateSwapChain + ResizeBuffers hooks. Smallest change, proven hook
  points, game adapts aspect from backbuffer size.
- B) A + output-mode spoofing (EnumModes/GetDisplayModeList): held as an
  evidence-gated fallback if the game fights approach A from its settings UI.
- C) Config-file only: static, game may ignore/clamp. Last-resort fallback.

## 4. Design

- Source of truth: per-eye recommended dims from
  `xrEnumerateViewConfigurationViews`; refresh = the runtime's CURRENT display
  rate (`XR_FB_display_refresh_rate`), env-overridable via `MECVR_FORCE_HZ`.
- Enforcement: desc rewrite in the shared hook layer (quad + probe builds
  behave identically). Dims + `RefreshRate` only; all other desc flags pass
  through untouched. Kill-switch: `MECVR_FORCE_RES=0` restores stock descs. Fail-closed: if the
  runtime values cannot be enumerated, descs pass through untouched.
- Observability: status line logs forced `WxH@Hz` plus the runtime values it
  was derived from, every session.
- Presentation: no quad shader change. The 1:1 upload path absorbs the larger
  frame (~36 MB at 4128×2208 ≈ 4 ms upload; re-measure live).
- Testing: unit test for the pure rewrite math (dims + Hz mapping, kill-switch,
  clamping); live check = forced res in log + full frame on quad + fps sanity.

## 5. Out of scope

- True HMD-paced present scheduling (game still self-paces; deferred to M5/M6).
- Approach B/C unless approach A fails live (evidence-gated).

## 6. Risks

- Game caches display modes and ignores the forced desc → fall back to B.
- Full-stereo-width render cost tanks fps on this GPU → user revisits D1 with
  measured fps in hand; kill-switch gives instant relief.
- Forcing windowed/fullscreen state is NOT done (flags untouched); if the game
  needs a mode change to accept the desc, that becomes a design revision.
