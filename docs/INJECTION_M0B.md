# MECVR M0B Safe Runtime Injection (T6, game stream)

Date: 2026-09-25. Scope: injection and lifecycle ONLY — `mecvr_runtime.dll`
loads into the live game with logging/IPC up and a bounded worker
lifecycle, with ZERO graphics hooks enabled (and none present in code).

Hard gates honored: no game-directory writes (verified by before/after
snapshot); no camera/stereo/gameplay code anywhere in T6 sources
(STOP S3 clean); bounded shutdown only, no `FreeLibrary` hot-unload path;
Frosty coexistence investigated (Frosty absent — see below); identical
game behavior verified by 60 s responsiveness watch + log.

## Files created (all new)

`src/bootstrap/` (T6-owned dir; `diag_main.cc` untouched):

- `runtime_worker.h` — worker contract: `%TEMP%\mecvr_runtime.log`
  location, fixed bounds (`kHeartbeatMs = 2000`,
  `kShutdownTimeoutMs = 5000`), per-PID IPC object names (named shutdown
  event, named pipe, shared-memory status block), `RuntimeStatus` POD.
  Zero graphics/camera/input/render declarations by design.
- `runtime.cc` — `mecvr_runtime.dll`: `DllMain` starts exactly one worker
  thread on `DLL_PROCESS_ATTACH` (`DisableThreadLibraryCalls` +
  `InterlockedCompareExchange` once-guard, `_beginthreadex` because the
  worker uses the CRT) and stops it on `DLL_PROCESS_DETACH` with a fixed
  5 s wait. The worker opens the file logger (`diagnostics/logging.h`
  pattern reused), logs `worker-started`, emits a `heartbeat` line every
  2 s, updates the shared-memory status block per heartbeat, and logs
  `worker-stopped` on exit. Exports: `MecvrRequestShutdown` (explicit
  shutdown) and `MecvrHeartbeatTick` (polling). No hook code exists.
- `inject.h` / `inject.cc` — `FindPidByName`, `WaitForPidByName`,
  `InjectDllByPid` (classic `CreateRemoteThread` + `LoadLibraryW` with an
  absolute DLL path), `SignalRuntimeShutdown` (signals the named event).
- `inject_main.cc` — `mecvr_inject.exe` CLI: `--dll PATH --pid N`,
  `--dll PATH --wait NAME [--timeout SEC]`, `--shutdown PID`, `--find NAME`.

No `CMakeLists.txt` modified, no `git` run, `build/` and `.empryo/`
untouched. Binaries were compiled to `%TEMP%\mecvr_t6\` (outside the repo
and outside the game dir); stray `.obj`/`.lib`/`.exp` files the compiler
dropped in the workspace root were deleted.

## Toolchain

MSVC via BuildTools 18 (`VsDevCmd.bat -arch=x64`),
`cl.exe /nologo /W4 /WX /EHsc /std:c++17 /MT` — both targets compile
warning-free with warnings-as-errors. `/MT` (static CRT) chosen so the
injected DLL has no CRT-DLL dependency inside the game process. DLL linked
with `/LD`; sources: `runtime.cc + diagnostics/logging.cc` and
`inject.cc + inject_main.cc` respectively, with `/I src`.

## Frosty check

`FrostyModManager` is NOT installed on this machine: no `*rosty*` files
under Downloads or the user profile root, no `FrostyModManager` command,
no running Frosty/game process at start. Nothing was installed (per task).
The `--wait NAME` attach path (`WaitForPidByName("MirrorsEdgeCatalyst.exe",
timeout)`) is implemented for a Frosty-launched game and the demonstrated
equivalent here is attach-to-running-game: the retail exe was launched
directly and injection performed by PID. Frosty coexistence under a real
Frosty launch remains open for T7.

## Validation log (live game, retail exe)

Game dir pre-snapshot: 337 files, max `LastWriteTimeUtc`
2026-06-15T09:37:04Z. Stale `%TEMP%\mecvr_runtime.log` removed.

1. Launch: `Start-Process .../MirrorsEdgeCatalyst.exe` (working dir =
   game dir, read-only). Visible main window within budget: pid 30756,
   `MainWindowHandle 2818234`, `Responding True`,
   title `Mirror's Edge™ Catalyst`.
2. Inject: `mecvr_inject.exe --dll %TEMP%\mecvr_t6\mecvr_runtime.dll
   --pid 30756` → `injected pid=30756`, exit 0.
3. DLL log after 7 s:
   `[INFO] worker-started pid=30756 ...`
   `[INFO] heartbeat tick=1..3 pid=30756` — worker + heartbeat confirmed.
4. Module check: `mecvr_runtime.dll` listed in the game process modules,
   loaded from `%TEMP%\mecvr_t6\` (nothing placed beside the game exe).
5. 60 s watch (10 s samples): `Responding True`, same HWND, same title at
   t=10..60 s — no crash, no hang, window behavior identical.
6. Explicit shutdown: `mecvr_inject.exe --shutdown 30756` → `shutdown
   signaled`, log tail shows heartbeats up to `tick=60` then
   `worker-stopped pid=30756 tick=60` — bounded worker exit confirmed.
   Game still alive and responsive after worker stop.
7. `Stop-Process 30756` → pid gone, zero `MirrorsEdgeCatalyst` processes
   remain — clean exit verified (exercises the `DLL_PROCESS_DETACH`
   stop path).
8. Game dir post-snapshot: 337 files, max `LastWriteTimeUtc`
   2026-06-15T09:37:04Z — identical: never written.

## STOP gates S1–S5

None triggered. S1: retail build is the known fingerprint from `BASELINE_M0A.md`
(87,780,864 bytes; no invasive hooks exist to gate). S2: no Present-hook
work in T6 (N/A). S3: no camera/stereo/gameplay code written or enabled.
S4: no unit-test scope in T6 (both binaries compile `/WX`-clean). S5: no
Frosty launch available (Frosty absent); no regression observed on the
vanilla-equivalent path.

## Deviations

- Build outputs placed in `%TEMP%\mecvr_t6\` instead of the repo or game
  dir: keeps the game dir read-only and the repo free of binaries while
  satisfying "no DLLs placed beside the game exe".
- `/MT` instead of the default `/MD`: self-contained DLL is safer for
  remote-process loading; noted above.
- Responsiveness proven via `Get-Process Responding` + stable HWND/title
  sampling rather than screenshot (screenshot explicitly not required).
- Full 60-heartbeat run (~2 min wall time incl. logging checks) rather
  than exactly 60 s of heartbeats; the 60 s identical-behavior watch
  itself is exact (t=10..60 s samples).
