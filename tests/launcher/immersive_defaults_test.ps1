$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script = Get-Content -Raw (Join-Path $repo 'tools\launch_preview.ps1')
$launcher = Get-Content -Raw (Join-Path $repo 'src\launcher\launcher_main.cc')

$failures = [System.Collections.Generic.List[string]]::new()
if (-not $script.Contains("[ValidateSet('quad', 'projection')] [string]`$MonoLayer = 'projection'")) {
  $failures.Add('launch_preview must default to immersive projection')
}
if (-not $script.Contains('[switch]$EnableStereo')) {
  $failures.Add('launcher must retain the stereo compatibility switch')
}
if (-not $launcher.Contains('ReadSetting(L"Stereo", L"1")')) {
  $failures.Add('launcher stereo default must be immersive')
}
if (-not $launcher.Contains('ReadSetting(L"Layer", L"projection")')) {
  $failures.Add('launcher presentation default must be projection')
}
if (-not $script.Contains("[ValidateSet('balanced', 'performance', 'diagnostic')]")) {
  $failures.Add('launcher must expose a performance profile')
}
if (-not $script.Contains('$env:MECVR_PERFORMANCE_MODE = $effectivePerformanceMode')) {
  $failures.Add('performance profile must reach the runtime')
}
if (-not $launcher.Contains('ReadSetting(L"SettingsVersion", L"1") != L"2"')) {
  $failures.Add('legacy launcher settings must migrate away from theatre defaults')
}
if ($failures.Count -ne 0) {
  $failures | ForEach-Object { Write-Error $_ }
  exit 1
}
Write-Output 'IMMERSIVE_DEFAULTS_PASS'
