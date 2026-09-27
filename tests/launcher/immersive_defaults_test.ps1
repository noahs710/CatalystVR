$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script = Get-Content -Raw (Join-Path $repo 'tools\launch_preview.ps1')
$launcher = Get-Content -Raw (Join-Path $repo 'src\launcher\launcher_main.cc')

$failures = [System.Collections.Generic.List[string]]::new()
if (-not $script.Contains("[ValidateSet('quad', 'projection')] [string]`$MonoLayer = 'projection'")) {
  $failures.Add('launch_preview must default to immersive projection')
}
if (-not $script.Contains('[switch]$DisableStereo')) {
  $failures.Add('stereo must be default-on with an explicit disable switch')
}
if (-not $launcher.Contains('ReadSetting(L"Stereo", L"1")')) {
  $failures.Add('launcher stereo default must be enabled')
}
if (-not $launcher.Contains('ReadSetting(L"Layer", L"projection")')) {
  $failures.Add('launcher presentation default must be projection')
}
if (-not $launcher.Contains('ReadSetting(L"SettingsVersion", L"1") != L"2"')) {
  $failures.Add('legacy launcher settings must migrate away from theatre defaults')
}
if ($failures.Count -ne 0) {
  $failures | ForEach-Object { Write-Error $_ }
  exit 1
}
Write-Output 'IMMERSIVE_DEFAULTS_PASS'
