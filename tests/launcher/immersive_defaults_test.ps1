$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script = Get-Content -Raw (Join-Path $repo 'tools\launch_preview.ps1')
$launcher = Get-Content -Raw (Join-Path $repo 'src\launcher\launcher_main.cc')

$failures = [System.Collections.Generic.List[string]]::new()
if (-not $script.Contains("[ValidateSet('quad', 'projection')] [string]`$MonoLayer = 'projection'")) {
  $failures.Add('launch_preview must default to immersive projection')
}
if (-not $script.Contains('[ValidateRange(512, 4096)] [int]$XrResolution = 2048')) {
  $failures.Add('launcher must default to a 2048 square XR target')
}
if (-not $script.Contains('$env:MECVR_XR_RESOLUTION')) {
  $failures.Add('XR resolution setting must reach the OpenXR backend')
}
if (-not $script.Contains('[switch]$EnableStereo')) {
  $failures.Add('launcher must retain the stereo compatibility switch')
}
if (-not $launcher.Contains('ReadSetting(L"Stereo", L"1")')) {
  $failures.Add('launcher stereo default must be immersive')
}
if (-not $launcher.Contains('ReadSetting(L"BodyOverlay", L"0")')) {
  $failures.Add('launcher IK body overlay must default to opt-in for performance')
}
if (-not $script.Contains("elseif (`$EnableBodyOverlay) { '1' } else { '0' }")) {
  $failures.Add('launcher IK body overlay must be opt-in')
}
if (-not $script.Contains('$nativePoseRequested = -not [string]::IsNullOrWhiteSpace($NativeBoneMap)')) {
  $failures.Add('native bone map must request palette correlation')
}
if (-not $script.Contains('$DiscoverPalettes -or $nativePoseRequested')) {
  $failures.Add('native bone map must reach palette discovery mode')
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
if (-not $launcher.Contains('ReadSetting(L"SettingsVersion", L"1") != L"4"')) {
  $failures.Add('legacy launcher settings must migrate away from theatre defaults')
}
if (-not $script.Contains('[switch]$CaptureNativeBoneMap')) {
  $failures.Add('launcher must expose one-shot native Faith contract capture')
}
if (-not $script.Contains('$env:MECVR_CAPTURE_NATIVE_BONE_MAP')) {
  $failures.Add('native Faith contract capture must reach the runtime')
}
if (-not $script.Contains('$CaptureNativeBoneMap')) {
  $failures.Add('native Faith contract capture must force palette discovery')
}
if (-not $launcher.Contains('CaptureNativeBoneMap')) {
  $failures.Add('GUI launcher must persist and pass native contract capture')
}
if ($failures.Count -ne 0) {
  $failures | ForEach-Object { Write-Error $_ }
  exit 1
}
Write-Output 'IMMERSIVE_DEFAULTS_PASS'
