# MECVR M0C2 Live DX11 Observation (T3B, render stream)

Date: 2026-09-25. Probe: `src/render/live_probe.cc` (shared-vtable
observation — the probe creates its own hidden-window swapchains in-process
and patches the per-class COM vtables, which simultaneously observes the
game's pre-existing swapchains with no memory scan and no startup race).
OBSERVATION ONLY: no rendering modification, no camera/stereo/gameplay code.
Driven with the T6 injector; game launched with working dir = game dir.

One root-cause fix during validation (not a workaround): the probe's hidden
window proc passed `nullptr` to `DefWindowProcW`, killing the worker thread
at window creation (log stopped after 2 lines, game unaffected). Fixed to
forward the real `hwnd`; revalidation below is post-fix.

## Live graphics facts (retail 1.0.3.47248, pid 5728)
- Primary swapchain (most presents): 3440x1440, R8G8B8A8_UNORM,
  exclusive fullscreen (`fullscreen=1`, `windowed=0`) on `\\.\\DISPLAY1`,
  HWND `0x690694` (`Mirror's Edge Catalyst`, visible, client 3440x1440).
- Present args: `sync=0 flags=0` (uncapped), `flags_nonzero=0`, `resizes=0`.
- Cadence over 4095-frame window: mean 5.000 ms, min 4.54, max 5.49
  (~200 fps), single render thread throughout.
- `state_fail=0` across ~18,000 observed Presents: the StateGuard subset
  (RTV0, DSV, viewports, rasterizer/blend/depth-stencil identity) survived
  every call — hooking is transparent to game rendering state.
- Game D3D11 device: feature level `0xb100` (11.1), immediate context
  recorded. Adapter: `AMD Radeon RX 9060 XT`, vendor `0x1002`,
  device `0x7590`, **LUID `0x00000000:0x0001a699`** — feeds the T10G
  adapter/LUID gate directly.
- 5 distinct swapchain vtables hooked (legacy IDXGISwapChain plus 1..4),
  passive counting; no hitch, crash, or regression observed (S2 clean).
- Frame capture: one 3440x1440 BGRA capture to
  `%TEMP%\\mecvr_capture_5728.bmp` (19,814,454 bytes, valid BM header,
  top-down), `hr=S_OK`, from the primary swapchain.
- Shutdown: game terminated on request post-observation; no surviving
  `MirrorsEdgeCatalyst.exe`; game dir never written.

## STOP gates
S1 clean (known retail hash). S2 clean (no instability). S3 clean (no
camera/stereo/gameplay code in probe). Post-fix log:
`%TEMP%\\mecvr_liveprobe_5728.log` (reviewed, then cleaned).
Capture BMP verified by header + dimensions.
