# MECVR Compatibility Scanner — T7 (game stream)

Date: 2026-09-26. Library: `src/compatibility/conflict_scan.h/.cc`,
test: `src/compatibility/conflict_scan_test.cc`.
Extends (does not modify) `src/compatibility/support_report.*` (T2):
reuses its `ModuleEntry` / `OverlayFinding` shapes; enumeration is
reimplemented for arbitrary PIDs because T2's collector lives in an
anonymous namespace and only runs on the process it launched.

## BEST-EFFORT — never exhaustive

This scanner detects **known** conflict signatures. It can never prove the
absence of an unknown Present hook: zero findings means "none of the known
signatures matched", **not** "no hooks exist". Every report header and the
`ToText()` output carry this label. Never quote a clean scan as proof of a
clean process.

## What it checks (read-only)

1. **Loaded-module enumeration** of a target PID (`EnumProcessModulesEx`,
   with a read-only Toolhelp snapshot fallback when `OpenProcess` is denied).
2. **Known-basename match** (best-effort list): ReShade (`reshade64/32.dll`,
   plus `dxgi/d3d11.dll` only when loaded from the game dir), SpecialK,
   RTSS hooks (`rtsshooks64.dll`), Steam overlay
   (`gameoverlayrenderer64.dll`), Discord overlay (`discordhook*.dll`),
   game-dir shim suspects (`dxgi.dll`, `d3d11.dll`, `dinput8.dll`,
   `dsound.dll`, `version.dll`, `winmm.dll` from the game dir).
   A System32 `dxgi.dll`/`d3d11.dll` is normal and is NOT flagged.
3. **DXGI export-prologue inspection where safe.** `Present`/`ResizeBuffers`
   are vtable methods, not exports, so no remote vtable resolution is
   attempted (that would require an in-process device). Instead the scanner
   does bounded read-only prologue reads of `CreateDXGIFactory/1/2`:
   remote address = remote `dxgi.dll` base + local export RVA, then
   `ReadProcessMemory` of 16 bytes. Prologues shaped like `E9 rel32`,
   `EB` at entry, `FF 25` at entry, or `PUSH+RET` stubs are reported as
   detour-like, with JMP-target ownership attributed against the remote
   module list. Anything it cannot read is reported as skipped, never
   touched. Local `dxgi.dll` prologue bytes are the clean reference.
4. **Detour-ownership notes**: a thunk landing inside a third-party module
   is suspicious; landing inside `dxgi.dll` itself is likely benign.

## What it never does

No injection, no hooks, no `WriteProcessMemory`, no remote threads, no game
writes, no window/input interference. The API returns findings; the **caller
decides**. Warnings never block. Only observed real instability
(hitch/crash/regression vs baseline) triggers STOP S2 — and that decision
belongs to the caller, not this library.

## Validation evidence (2026-09-26)

Toolchain: MSVC via BuildTools 18 (Developer Prompt v18.7.2),
`cl /std:c++17 /W4 /WX /EHsc`, linked `psapi.lib`. Clean compile, zero
warnings. Test binary built with `cl.exe` directly (no CMake changes).

- **Self-test**: 17/17 checks pass — synthetic matcher cases (ReShade, RTSS,
  SpecialK, Steam overlay flagged; game-dir `dxgi.dll`/`dinput8.dll` shims
  flagged; System32 `dxgi.dll` ignored), prologue-shape cases (E9/EB/FF25/
  PUSH+RET flagged; normal `mov` prologue and null input pass), self-scan
  completes, enumerates own modules (sees `kernel32.dll`), inspects local
  dxgi prologues (all clean, e.g. `48 89 5c 24 08 57 ...`).
- **Live-game run**: retail `MirrorsEdgeCatalyst.exe` launched with working
  dir = game dir (pid 10288, window `525998` up, ~104 modules),
  read-only `ScanPid(10288)` → `scan_ok=1`, 104 modules (5 game-dir:
  exe, `NvCameraSDK64.dll`, `ItsAMe_Origin.dll`, `dbdata.dll`,
  `Engine.BuildInfo_Win64_retail.dll` — consistent with the T2 baseline),
  known-hits = 0, all three remote DXGI prologues `read_ok=1`,
  byte-identical to the local clean reference, `detour_like=0`,
  detour notes = 0. `ALL CHECKS PASSED`, exit 0.
- **Shutdown**: game ignored `WM_CLOSE`, force-terminated, verified gone
  (`Get-Process` empty). Game dir never written.

## Known environment quirk (documented, not a code defect)

Binaries executed from the agent sandbox temp dir get `OpenProcess`/
snapshot `gle=5` even against benign processes (e.g. `explorer.exe`);
the identical binary under the workspace (`mecvr_t7_tmp/`) scans fine.
`tasklist /m` corroborated the live module list independently. No DRM
handle protection was observed — T2's parent-handle path and this PID path
agree. The Toolhelp fallback stays in the code for genuinely protected
processes.

## STOP gates

S1–S5: none triggered. No unknown build (retail only, hash per T2
baseline); no instability observed (S2 N/A — scan is read-only and the game
ran normally throughout); no camera/stereo/gameplay code (S3 clean); all
scanner checks green (S4 satisfied); no Frosty path exercised — Frosty
coexistence remains open for T6.
