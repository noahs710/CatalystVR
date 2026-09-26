# GAME_BUILDS.md — toolchain record + known executable fingerprints

## Toolchain (verified 2026-09-25, T1)
- cmake 4.4.3
- MSVC 14.51.36231 (`18\BuildTools\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe`)
- Windows SDK 10.0.26100.0 (latest installed; older 10.0.14393.0–10.0.17134.0 also present)
- git 2.54.0.windows.1
- Active OpenXR runtime: Virtual Desktop Streamer (`virtualdesktop-openxr.json`)

## Known executables (hashes pending T2/M0A)
| Build | Path | SHA-256 | Version | Adapter evidence |
|---|---|---|---|---|
| Retail (offline) | `MirrorsEdgeCatalyst.exe` (87,780,864 B) | `b1b6acf3…9f790` (full in `BASELINE_M0A.md`) | 1.0.3.47248 | TBD T3B |
| Trial | `MirrorsEdgeCatalystTrial.exe` (110,540,720 B) | `519957ac…41d8` (full in `BASELINE_M0A.md`) | 1.0.3.47248 | TBD T3B, only if executed |

Retail and Trial are fingerprinted independently; neither borrows the other's
hook evidence (spec §2).

## Launch note (verified T6 integration)
The exe must be launched with working directory = game dir (it spawns a child
`MirrorsEdgeCatalyst.exe`; inject into the windowed child, not the stub).
Direct launch without CWD exits quickly with no surviving process.
