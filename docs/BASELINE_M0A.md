# MECVR M0A Static/Process Baseline (T2, game stream)

Date: 2026-09-25. Tool: interim `mecvr_diag.exe --report`
(`src/bootstrap/diag_main.cc` + `src/compatibility/support_report.*`).
The real `MECVR.exe` launcher arrives in a later task; this CLI only prints
the static/process section of the support report.

Scope: externally observable facts ONLY — on-disk exe identity, live
process/window/module survey. No D3D11 device/swapchain/cadence claims
(T3B owns those). The game directory was never written; the game was
launched read-only via `CreateProcess` (no injection, hook, or patch),
politely closed, and exit verified. Trial exe was hashed from disk only,
never launched.

Toolchain: MSVC via BuildTools 18 (Developer Prompt v18.7.2),
`cl /W4 /WX /EHsc /std:c++17`, linked `version.lib psapi.lib user32.lib`.
Built with `cl.exe` directly; `CMakeLists.txt`, `build/`, and
`docs/GAME_BUILDS.md` (T1 record) untouched.

## On-disk fingerprints

- `MirrorsEdgeCatalyst.exe`: exists, 87,780,864 bytes,
  SHA-256 `b1b6acf3ca720522c1a329a0ad086335440fd89adf9114f8c636d1546289f790`,
  file/product version `1.0.3.47248`.
- `MirrorsEdgeCatalystTrial.exe`: exists, 110,540,720 bytes,
  SHA-256 `519957aca74fca8343c126265642b9ecef4acbc5495c4317e60eb8855c2412d8`,
  file/product version `1.0.3.47248`.
- Retail-vs-Trial: distinct builds (different size and hash, same version
  string). Each build must be fingerprinted/adapted independently (S1).

## Live process baseline (retail, tool-launched 2026-09-25)

- Visible main window found within wait budget (pid 36780, parent
  `mecvr_diag.exe` — not FrostyModManager; vanilla-equivalent launch path).
- HWND `0x1709ae`, title `"Mirror's Edge™ Catalyst"`,
  class `"Mirror's Edge™ Catalyst"`, client area 1280x720.
- Window mode: `windowed` — evidence `style=0x14cf0000 exstyle=0x00000100
  popup=0 overlappedwindow=1 client=1280x720 window=1296x759
  monitor=2752x1152`.
- Loaded modules: 93 enumerated via `EnumProcessModulesEx`. Game-dir
  modules: `MirrorsEdgeCatalyst.exe`, `NvCameraSDK64.dll`,
  `ItsAMe_Origin.dll`, `dbdata.dll`, `Engine.BuildInfo_Win64_retail.dll`.
  System `dxgi.dll`/`d3d11.dll` loaded from `C:\WINDOWS\SYSTEM32` (not
  game-dir proxies). Full list in the tool output below.
- Overlay/injector/proxy screen (best-effort basename list — ReShade,
  RTSS hooks, SpecialK, Steam/Discord overlays, game-dir
  dxgi/d3d11/dinput8 shims): 0 findings. Also of note: Logitech G HUB
  `sdk_legacy_led_x64.dll` and Defender `MpOav.dll` present; neither is on
  the known-bad list.
- Shutdown: `terminated_cleanly=1` (WM_CLOSE accepted); post-run
  `Get-Process MirrorsEdgeCatalyst` shows no remaining process.

## STOP gates

S1–S5: none triggered. Game launched and exited normally; no unknown
build (retail hash recorded above); no Present-hook work (S2 N/A);
no camera/stereo/gameplay code (S3 clean); no unit-test scope in T2
(S4 N/A); no Frosty path exercised — Frosty coexistence remains open
for T6/T7.

## Full tool output (live run, exit=0)

```text
MECVR M0A static/process baseline
game_dir=C:\Users\Gabrielle Monlea\Downloads\Mirrors Edge Catalyst
[exe] name=MirrorsEdgeCatalyst.exe exists=1 size=87780864 sha256=b1b6acf3ca720522c1a329a0ad086335440fd89adf9114f8c636d1546289f790 file_version=1.0.3.47248 product_version=1.0.3.47248
[exe] name=MirrorsEdgeCatalystTrial.exe exists=1 size=110540720 sha256=519957aca74fca8343c126265642b9ecef4acbc5495c4317e60eb8855c2412d8 file_version=1.0.3.47248 product_version=1.0.3.47248
retail-vs-trial: distinct builds (retail 87780864 bytes vs trial 110540720 bytes)
[live] attempted=1 found_window=1 pid=36780 ppid=25708 parent=mecvr_diag.exe
[window] hwnd=0x1709ae title="Mirror's Edge™ Catalyst" class="Mirror's Edge™ Catalyst" client=1280x720 mode=windowed
  evidence: style=0x14cf0000 exstyle=0x00000100 popup=0 overlappedwindow=1 client=1280x720 window=1296x759 monitor=2752x1152 => windowed
[modules] count=93
  [game-dir] C:\Users\Gabrielle Monlea\Downloads\Mirrors Edge Catalyst\MirrorsEdgeCatalyst.exe
  [system]  C:\WINDOWS\SYSTEM32\ntdll.dll
  [system]  C:\WINDOWS\System32\KERNEL32.DLL
  [system]  C:\WINDOWS\System32\KERNELBASE.dll
  [system]  C:\WINDOWS\SYSTEM32\apphelp.dll
  [system]  C:\WINDOWS\System32\ADVAPI32.dll
  [system]  C:\WINDOWS\System32\msvcrt.dll
  [system]  C:\WINDOWS\System32\sechost.dll
  [system]  C:\WINDOWS\System32\RPCRT4.dll
  [system]  C:\WINDOWS\System32\CRYPT32.dll
  [system]  C:\WINDOWS\System32\ucrtbase.dll
  [system]  C:\WINDOWS\System32\GDI32.dll
  [system]  C:\WINDOWS\System32\win32u.dll
  [system]  C:\WINDOWS\System32\gdi32full.dll
  [system]  C:\WINDOWS\SYSTEM32\DINPUT8.dll
  [system]  C:\WINDOWS\System32\msvcp_win.dll
  [system]  C:\WINDOWS\System32\USER32.dll
  [system]  C:\WINDOWS\System32\OLEAUT32.dll
  [system]  C:\WINDOWS\System32\combase.dll
  [system]  C:\WINDOWS\SYSTEM32\IPHLPAPI.DLL
  [system]  C:\WINDOWS\System32\PSAPI.DLL
  [system]  C:\WINDOWS\System32\SHELL32.dll
  [system]  C:\WINDOWS\System32\SHLWAPI.dll
  [game-dir] C:\Users\Gabrielle Monlea\Downloads\Mirrors Edge Catalyst\NvCameraSDK64.dll
  [system]  C:\WINDOWS\System32\WS2_32.dll
  [system]  C:\WINDOWS\System32\ole32.dll
  [system]  C:\WINDOWS\SYSTEM32\USP10.dll
  [system]  C:\WINDOWS\SYSTEM32\VERSION.dll
  [system]  C:\WINDOWS\SYSTEM32\MSVCP120.dll
  [system]  C:\WINDOWS\SYSTEM32\MSVCR120.dll
  [system]  C:\WINDOWS\SYSTEM32\WINMM.dll
  [system]  C:\WINDOWS\SYSTEM32\WSOCK32.dll
  [system]  C:\WINDOWS\SYSTEM32\WTSAPI32.dll
  [system]  C:\WINDOWS\SYSTEM32\bcrypt.dll
  [system]  C:\WINDOWS\SYSTEM32\d3d11.dll
  [system]  C:\WINDOWS\SYSTEM32\dbghelp.dll
  [system]  C:\WINDOWS\SYSTEM32\dwmapi.dll
  [system]  C:\WINDOWS\SYSTEM32\dxgi.dll
  [system]  C:\WINDOWS\SYSTEM32\urlmon.dll
  [system]  C:\WINDOWS\SYSTEM32\SspiCli.dll
  [system]  C:\WINDOWS\SYSTEM32\powrprof.dll
  [system]  C:\WINDOWS\SYSTEM32\iertutil.dll
  [system]  C:\WINDOWS\SYSTEM32\windows.storage.dll
  [system]  C:\WINDOWS\SYSTEM32\srvcli.dll
  [system]  C:\WINDOWS\SYSTEM32\netutils.dll
  [system]  C:\WINDOWS\SYSTEM32\dbgcore.DLL
  [system]  C:\WINDOWS\System32\IMM32.DLL
  [system]  C:\WINDOWS\SYSTEM32\UMPDC.dll
  [game-dir] C:\Users\Gabrielle Monlea\Downloads\Mirrors Edge Catalyst\ItsAMe_Origin.dll
  [system]  C:\WINDOWS\SYSTEM32\inputhost.dll
  [system]  C:\WINDOWS\SYSTEM32\CoreMessaging.dll
  [system]  C:\WINDOWS\System32\shcore.dll
  [system]  C:\WINDOWS\system32\mswsock.dll
  [system]  C:\WINDOWS\System32\bcryptPrimitives.dll
  [system]  C:\WINDOWS\SYSTEM32\CRYPTBASE.DLL
  [system]  C:\WINDOWS\SYSTEM32\kernel.appcore.dll
  [system]  C:\WINDOWS\SYSTEM32\profapi.dll
  [game-dir] C:\Users\Gabrielle Monlea\Downloads\Mirrors Edge Catalyst\dbdata.dll
  [system]  C:\WINDOWS\SYSTEM32\cabinet.dll
  [system]  C:\WINDOWS\system32\uxtheme.dll
  [game-dir] C:\Users\Gabrielle Monlea\Downloads\Mirrors Edge Catalyst\Engine.BuildInfo_Win64_retail.dll
  [system]  C:\WINDOWS\System32\clbcatq.dll
  [system]  C:\WINDOWS\system32\wbem\wbemprox.dll
  [system]  C:\WINDOWS\SYSTEM32\wbemcomn.dll
  [system]  C:\WINDOWS\system32\wbem\wbemsvc.dll
  [system]  C:\WINDOWS\system32\wbem\fastprox.dll
  [system]  C:\WINDOWS\SYSTEM32\amsi.dll
  [system]  C:\WINDOWS\SYSTEM32\USERENV.dll
  [system]  C:\ProgramData\Microsoft\Windows Defender\Platform\4.18.26080.4-0\MpOav.dll
  [system]  C:\Program Files\LGHUB\sdks\sdk_legacy_led_x64.dll
  [system]  C:\WINDOWS\SYSTEM32\MSVCP140.dll
  [system]  C:\WINDOWS\SYSTEM32\VCRUNTIME140.dll
  [system]  C:\WINDOWS\SYSTEM32\VCRUNTIME140_1.dll
  [system]  C:\WINDOWS\SYSTEM32\dxcore.dll
  [system]  C:\WINDOWS\SYSTEM32\directxdatabasehelper.dll
  [system]  C:\WINDOWS\SYSTEM32\cfgmgr32.dll
  [system]  C:\WINDOWS\SYSTEM32\PROPSYS.dll
  [system]  C:\WINDOWS\SYSTEM32\DEVOBJ.dll
  [system]  C:\WINDOWS\System32\WINTRUST.dll
  [system]  C:\WINDOWS\SYSTEM32\MSASN1.dll
  [system]  C:\WINDOWS\SYSTEM32\winspool.drv
  [system]  C:\WINDOWS\System32\MSCTF.dll
  [system]  C:\WINDOWS\SYSTEM32\textinputframework.dll
  [system]  C:\WINDOWS\system32\Oleacc.dll
  [system]  C:\WINDOWS\SYSTEM32\WINSTA.dll
  [system]  C:\WINDOWS\SYSTEM32\xinput1_4.dll
  [system]  C:\WINDOWS\SYSTEM32\HID.DLL
  [system]  C:\WINDOWS\System32\SETUPAPI.dll
  [system]  C:\WINDOWS\SYSTEM32\ntmarta.dll
  [system]  C:\WINDOWS\System32\DriverStore\FileRepository\u0203304.inf_amd64_a6e5a337568ce3f0\B026373\amdxx64.dll
  [system]  C:\WINDOWS\system32\atidxx64.dll
  [system]  C:\WINDOWS\System32\DriverStore\FileRepository\u0203304.inf_amd64_a6e5a337568ce3f0\B026373\amdenc64.dll
  [system]  C:\WINDOWS\System32\DriverStore\FileRepository\u0203304.inf_amd64_a6e5a337568ce3f0\B026373\amdihk64.dll
[overlays] findings=0
  none of the known basenames matched (best-effort only)
[launcher] parent is mecvr_diag.exe (not FrostyModManager)
[shutdown] terminated_cleanly=1
scope-note: static/process facts only; no D3D11 claims (T3B owns).
```
