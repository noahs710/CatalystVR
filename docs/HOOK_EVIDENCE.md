# MECVR HOOK_EVIDENCE.md (T10, render stream; T3B facts incorporated)

Format per spec §1: exe hash/version, module, signature/hook point,
hypothesis, discovery, observed evidence, validation, failure behavior,
confidence. Pre/post-HUD: prove-or-mark-unresolved (M2 needs transport
only, not HUD separation).

## E1 — Present-time backbuffer IS the final-present output [PROVEN, high]
- Exe: `MirrorsEdgeCatalyst.exe` SHA-256
  `b1b6acf3ca720522c1a329a0ad086335440fd89adf9114f8c636d1546289f790`,
  version 1.0.3.47248. Module: system `dxgi.dll` (no game proxy).
- Hook point: shared COM vtables `IDXGISwapChain::Present[8]` /
  `ResizeBuffers[13]` + `ID3D11DeviceContext::OMSetRenderTargets[33]`
  (slot 33 proven by in-process 64x64-RTV sentinel, OBSERVED).
- Hypothesis: the RTV bound at Present time (RTV0) is the backbuffer the
  game presents.
- Discovery: per-Present `OMGetRenderTargets` snapshot + `GetBuffer(0)`
  identity test in `src/render/m2a_probe.cc` (shared-vtable technique
  from T3B, no memory scan, no startup race).
- Observed (live pid 5872): `backbuffer_identity match=9929 mismatch=0
  unknown=1` over 9930 presents; RTV0 3440x1440 every call.
- Validation test: sustained 100%-minus-noise match rate across the run;
  falsification = any run with mismatch>1% or RTV0 size != swapchain size.
- Failure behavior: mark UNRESOLVED, do not assume finality.
- Confidence: high. M2A capture point = Present-time backbuffer: VALID.

## E2 — Hook transparency / S2 gate [PROVEN, high]
- Observed: `state_fail=0` across all presents (StateGuard subset: RTV0,
  DSV, viewports, rasterizer/blend/depth-stencil identity); in-hook added
  work mean 5.47 us against a ~5.0 ms frame (0.1%).
- Cadence: steady ~5.0-5.2 ms (~193-200 fps), single render thread; one
  140 ms max sample during the load phase, no sustained regression vs the
  T3B 5.000 ms baseline. Game ran normally throughout; terminated on
  request. S2: clean.
- Falsification: state_fail>0 or sustained mean shift >10% vs baseline.

## E3 — Primary swapchain identity (T3B carry-over) [PROVEN, high]
- Single primary: 3440x1440 R8G8B8A8_UNORM exclusive fullscreen on
  DISPLAY1, uncapped (sync=0/flags=0), one ResizeBuffers observed
  (count=3 1280x720, startup mode settle). Game D3D11 FL 11.1, adapter
  `AMD RX 9060 XT` LUID `0x00000000:0x0001a699` (see OBSERVER_M0C2.md;
  feeds T10G).

## E4 — Pre/post-HUD boundary [UNRESOLVED, explicit]
- Attempted: per-frame OMSetRenderTargets ring + last-fullscreen-before-
  Present analysis + two time-separated captures with pixel stats.
- Observed: the game issued ZERO per-frame OMSetRenderTargets calls (only
  the probe's own sentinel, on a worker thread) — targets are bound once
  at startup and reused, so no per-frame HUD-composite event is
  observable; `fs_found=0` across the window.
- Early capture: cap1-early ok, 3440x1440, FNV `0xd4ce95300078e17e`,
  mean_luma 247.16 (near-white loading screen). cap2-late never triggered
  (30k-present threshold not reached in the window). Capture BMP files did
  not materialize on disk (stats recorded in-log only) — limitation noted,
  does not affect the identity conclusion.
- Falsification test for a future claim: toggle HUD state and show the
  pre/post captures diverge with statistical significance; until then no
  pre-HUD capture point may be used. HUD conversion remains forbidden
  (STOP S3). M2 proceeds on the proven final-output capture.

## E5 — OMSetRenderTargets slot 33 [PROVEN, medium]
- Sentinel: 3/3 own-context 64x64 RTV calls observed in-ring; `broken=no`.
- Falsification: sentinel missing while other OM calls appear.
