# DX11 Observer M0C1 — Synthetic Render Stream (T3A)

Scope: synthetic ONLY. No live game process was attached to, launched, or
referenced. Live observation is T3B and is explicitly out of scope here.

## Files

- `src/render/vtable_hook.h` / `src/render/vtable_hook.cc` — vtable-patch
  hook utility (RAII unhook restores the original function pointer).
- `src/render/observer.h` / `src/render/observer.cc` — `Observer` class
  (Present/ResizeBuffers interception records) + `StateGuard` (D3D11
  immediate-context snapshot/verify).
- `src/render/synthetic_test.cc` — synthetic test exe: real D3D11 device +
  swapchain on a hidden window, hook install, Present/ResizeBuffers calls,
  assertions on interception data, state preservation, and clean unhook.

## Hook technique: vtable patching (permanent, not a hack)

Each `VtableHook::install` does exactly two things: `VirtualProtect` the
one vtable slot's page to `PAGE_READWRITE`, then one pointer-sized write
swapping in the detour. `uninstall` (also the destructor path) writes back
the exact original pointer and restores the page protection. Properties
that make this the permanent observer technique:

- No disassembler, no prologue decoding, no instruction relocation — the
  target address is read from the object's own vtable slot, so there is no
  fragile machine-code parsing to rot across builds.
- Bounded work per slot: one protection change + one pointer write, each
  reversed symmetrically on unhook.
- Trivially reversible: after unhook the vtable is bit-identical to before
  install (asserted by the test comparing slot contents to the captured
  originals), leaving no trace in the observed process.

Hooked slots (`observer.cc`, documented at the constants): `Present` = 8,
`ResizeBuffers` = 13, per the `IUnknown[0..2]` / `IDXGIObject[3..6]` /
`IDXGIDeviceSubObject[7]` / `IDXGISwapChain[8..]` layout.

## Observer records

Per call the detour records parameters (`SyncInterval`/`Flags`,
`BufferCount`/`Width`/`Height`/`Format`/`Flags`), calling thread id
(`GetCurrentThreadId`), a `steady_clock` timestamp, the original call's
`HRESULT`, and a `state_preserved` flag from `StateGuard`. Records are
mutex-guarded; the original is invoked without holding the lock. At most
one `Observer` may be installed at a time (second install fails closed);
the vtable is shared per COM class, so while installed every swapchain on
that vtable is observed — fine for the synthetic harness, to be revisited
for multi-swapchain handling in T3B.

## StateGuard subset

Snapshots before the original call, re-queries after, compares:

- OM render targets: first render-target slot + depth-stencil view
  (object identity, null included).
- RS viewports: bound count + first viewport (all six fields).
- RS rasterizer state, OM blend state, OM depth-stencil state (object
  identity) + blend factor (4 floats), sample mask, stencil ref.

## Build and run

MSVC 19.51.36248 (x64) via VsDevCmd, `/W4 /WX /EHsc /std:c++17`, link
`d3d11.lib dxgi.lib user32.lib`. Object/exe outputs go to `%TEMP%` —
never `build/`, and `CMakeLists.txt` is untouched.

## Evidence (2026-09-25 run, `observer_test.exe`, exit 0)

```text
main thread id: 33780
PASS: hidden window created (never shown)
PASS: D3D11 device + swapchain created
PASS: back buffer acquired
PASS: render-target view created
PASS: depth texture created
PASS: depth-stencil view created
PASS: rasterizer state created
PASS: blend state created
PASS: depth-stencil state created
PASS: observer hook installed
PASS: observer reports installed
PASS: second install fails closed
PASS: Present(0,0) #1 returned success
PASS: ResizeBuffers returned success
PASS: post-resize back buffer acquired
PASS: post-resize render-target view created
PASS: Present(0,0) #2 returned success
PASS: two Present interceptions recorded
PASS: one ResizeBuffers interception recorded
PASS: Present #1 parameters recorded
PASS: Present #2 parameters recorded
PASS: Present calling thread id matches
PASS: Present HRESULTs recorded
PASS: D3D11 state preserved across Present hooks
PASS: Present timestamps monotonic
PASS: ResizeBuffers parameters recorded
PASS: ResizeBuffers calling thread id matches
PASS: ResizeBuffers HRESULT recorded
PASS: D3D11 state preserved across ResizeBuffers hook
PASS: original function pointers captured
PASS: observer unhooked cleanly
PASS: observer reports uninstalled
PASS: Present vtable slot restored to original
PASS: ResizeBuffers vtable slot restored to original
PASS: post-unhook Present works
PASS: no interceptions recorded after unhook
ALL CHECKS PASSED
```

One test-sequence fix during development: the first run called
`ResizeBuffers` while the back-buffer RTV was still bound, and DXGI
correctly refused (`DXGI_ERROR_INVALID_CALL`). The test now unbinds and
releases the RTV before resizing and rebinds a fresh RTV after — a test
harness correction, not an observer change.

## STOP assessment

No STOP triggered: no unknown builds touched (S1 — synthetic device only,
no game exe involved), no instability (S2 — all state-preservation checks
pass), no camera/stereo/gameplay code (S3), no red tests remain (S4), no
Frosty/game launch of any kind (S5).

## T3B handoff note

Single-active-observer limit and first-RTV-slot/first-viewport subset are
adequate for M0C1; T3B (live, multi-swapchain) should re-evaluate both.
