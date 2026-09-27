$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script = Get-Content -Raw (Join-Path $repo 'tools\launch_preview.ps1')
if (-not $script.Contains('--launch-suspended')) {
  Write-Error 'direct launch must inject before Catalyst renderer initialization'
  exit 1
}
if ($script.Contains('Start-Sleep -Seconds 3')) {
  Write-Error 'late three-second injection window must be removed'
  exit 1
}
Write-Output 'EARLY_INJECTION_PASS'
