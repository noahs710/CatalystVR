[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)] [string]$GamePath,
  [string]$PackageRoot,
  [ValidateSet('mono', 'camera')] [string]$Mode = 'camera',
  [ValidateSet('direct', 'frosty')] [string]$LaunchBackend = 'direct',
  [string]$FrostyPath,
  [string]$FrostyPack,
  [ValidateRange(10, 300)] [int]$FrostyTimeoutSeconds = 90,
  [switch]$EnableCamera,
  [switch]$EnableInput,
  [switch]$EnableStereo,
  [switch]$DisableStereo,
  [switch]$EnableBodyOverlay,
  [switch]$DisableBodyOverlay,
  [switch]$DisablePhysicalJump,
  [switch]$DisablePhysicalCrouchInput,
  [switch]$EnableParkourInput,
  [string]$ParkourVaultScan = '57',
  [string]$ParkourClimbScan = '18',
  [string]$ParkourSlideScan = '29',
  [string]$RecordMotionClip,
  [string]$PlayMotionClip,
  [switch]$DiscoverPalettes,
  [ValidateSet('quad', 'projection')] [string]$MonoLayer = 'projection',
  [ValidateSet('view', 'local')] [string]$QuadSpace = 'view',
  [ValidateSet('smooth', 'snap')] [string]$TurnMode = 'smooth',
  [ValidateSet('balanced', 'performance', 'diagnostic')] [string]$PerformanceMode = 'performance',
  [switch]$PreserveRuntimePacing,
  [string]$NativeBoneMap,
  [float]$UnitsPerMeter = 100.0
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($PackageRoot)) {
  $PackageRoot = Join-Path $PSScriptRoot '..\dist\MECVR-1.5.1.21-alpha'
}
$root = (Resolve-Path $PackageRoot).Path
$bin = Join-Path $root 'bin'
$injector = Join-Path $bin 'mecvr_inject.exe'
$loader = Join-Path $bin 'openxr_loader.dll'
$mod = if ($Mode -eq 'camera') { Join-Path $bin 'mecvr_m3b_live.dll' } else { Join-Path $bin 'mecvr_m2b_live.dll' }
foreach ($path in @($GamePath, $injector, $loader, $mod)) {
  if (-not (Test-Path -LiteralPath $path)) { throw "Missing path: $path" }
}
if (-not [string]::IsNullOrWhiteSpace($NativeBoneMap) -and
    -not (Test-Path -LiteralPath $NativeBoneMap -PathType Leaf)) {
  throw "Native bone contract does not exist: $NativeBoneMap"
}
if ($LaunchBackend -eq 'frosty') {
  if ([string]::IsNullOrWhiteSpace($FrostyPath) -or
      -not (Test-Path -LiteralPath $FrostyPath -PathType Leaf)) {
    throw 'Frosty backend requires an existing FrostyModManager.exe path.'
  }
  if ([string]::IsNullOrWhiteSpace($FrostyPack)) {
    throw 'Frosty backend requires a non-empty Frosty pack name.'
  }
}

$env:MECVR_ENABLE_CAMERA = if ($EnableCamera -or $Mode -eq 'camera') { '1' } else { '0' }
$env:MECVR_ENABLE_INPUT = if ($EnableInput) { '1' } else { '0' }
# Immersive stereo is the public launcher default. The current producer pairs
# consecutive game presents with an explicit epoch/pose gate; -DisableStereo
# remains available as a performance diagnostic and mono fallback.
$env:MECVR_ENABLE_STEREO = if ($DisableStereo) { '0' } else { '1' }
# Mod-owned IK arms/body are the public default; retain an explicit low-cost
# fallback for diagnostics and weak GPUs.
$env:MECVR_ENABLE_BODY_OVERLAY = if ($DisableBodyOverlay) { '0' } else { '1' }
$env:MECVR_ENABLE_PHYSICAL_JUMP = if ($DisablePhysicalJump) { '0' } else { '1' }
$env:MECVR_ENABLE_PHYSICAL_CROUCH_INPUT =
  if ($DisablePhysicalCrouchInput) { '0' } else { '1' }
$env:MECVR_ENABLE_PARKOUR_INPUT =
  if ($EnableParkourInput) { '1' } else { '0' }
$env:MECVR_PARKOUR_VAULT_SCAN = $ParkourVaultScan
$env:MECVR_PARKOUR_CLIMB_SCAN = $ParkourClimbScan
$env:MECVR_PARKOUR_SLIDE_SCAN = $ParkourSlideScan
$env:MECVR_RECORD_MOTION_CLIP = $RecordMotionClip
$env:MECVR_PLAY_MOTION_CLIP = $PlayMotionClip
$nativePoseRequested = -not [string]::IsNullOrWhiteSpace($NativeBoneMap)
$env:MECVR_DISCOVER_PALETTES =
  if ($DiscoverPalettes -or $nativePoseRequested) { '1' } else { '0' }
$env:MECVR_TURN_MODE = $TurnMode
$effectivePerformanceMode =
  if (($DiscoverPalettes -or $nativePoseRequested) -and
      $PerformanceMode -eq 'performance') {
    'diagnostic'
  } else {
    $PerformanceMode
  }
$env:MECVR_PERFORMANCE_MODE = $effectivePerformanceMode
$env:MECVR_PRESERVE_RUNTIME_PACING = if ($PreserveRuntimePacing) { '1' } else { '0' }
$env:MECVR_NATIVE_BONE_MAP = $NativeBoneMap
$env:MECVR_UNITS_PER_METER = $UnitsPerMeter.ToString([Globalization.CultureInfo]::InvariantCulture)
# Theatre presentation requires an explicit diagnostic argument and is never
# inherited from a stale process environment.
$env:MECVR_MONO_LAYER = if ($MonoLayer -eq 'quad') { 'quad' } else { 'projection' }
$env:MECVR_QUAD_SPACE = $QuadSpace
$resolvedGame = (Resolve-Path $GamePath).Path
$normalizedGame = [IO.Path]::GetFullPath($resolvedGame).TrimEnd('\').ToLowerInvariant()
function Get-CatalystPids([string]$ExactPath) {
  foreach ($row in (Get-CimInstance Win32_Process -Filter "Name='MirrorsEdgeCatalyst.exe'")) {
    if ([string]::IsNullOrWhiteSpace($row.ExecutablePath)) { continue }
    $candidate = [IO.Path]::GetFullPath($row.ExecutablePath).TrimEnd('\').ToLowerInvariant()
    if ($candidate -eq $ExactPath) { [int]$row.ProcessId }
  }
}

$game = $null
$frosty = $null
$attached = $false
try {
  if (@(Get-CatalystPids $normalizedGame).Count -gt 0) {
    throw 'A matching Catalyst process is already running; refusing ambiguous injection.'
  }
  if ($LaunchBackend -eq 'direct') {
    # Install renderer hooks before Catalyst creates any D3D contexts. This is
    # required to observe the scene command stream and eventually render both
    # eyes from one prepared simulation epoch.
    $launchOutput = @(& $injector --launch-suspended $resolvedGame `
        --cwd (Split-Path $resolvedGame) --dll $loader --dll $mod)
    if ($LASTEXITCODE -ne 0) { throw 'Suspended launch or preload failed.' }
    $pidLine = $launchOutput | Where-Object { $_ -match '^launched pid=([0-9]+)$' } |
      Select-Object -Last 1
    if ($null -eq $pidLine -or $pidLine -notmatch '^launched pid=([0-9]+)$') {
      throw 'Injector did not return the suspended Catalyst PID.'
    }
    $targetPid = [int]$Matches[1]
    $game = Get-Process -Id $targetPid -ErrorAction Stop
    $attached = $true
  } else {
    $frostyResolved = (Resolve-Path $FrostyPath).Path
    $frostyArgs = '-launch "' + $FrostyPack.Replace('"', '\"') + '"'
    $baseline = @((Get-CatalystPids $normalizedGame))
    $frosty = Start-Process -FilePath $frostyResolved -WorkingDirectory (Split-Path $frostyResolved) -ArgumentList $frostyArgs -PassThru
    Write-Output "Frosty backend launched pack '$FrostyPack'; waiting for exact Catalyst executable."
    $deadline = [DateTime]::UtcNow.AddSeconds($FrostyTimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
      $candidate = @(Get-CatalystPids $normalizedGame) |
        Where-Object { $baseline -notcontains $_ } | Select-Object -First 1
      if ($null -ne $candidate) {
        $game = Get-Process -Id ([int]$candidate) -ErrorAction Stop
        break
      }
      Start-Sleep -Milliseconds 500
    }
    if ($null -eq $game) {
      throw "Frosty did not launch the configured Catalyst executable within $FrostyTimeoutSeconds seconds."
    }
  }
  $targetPid = $game.Id
  if ($LaunchBackend -ne 'direct') {
    # Frosty owns process creation, so this backend attaches after its exact
    # executable-path match. Direct launch uses the pre-resume path above.
    & $injector --dll $loader --pid $targetPid
    if ($LASTEXITCODE -ne 0) { throw 'OpenXR loader injection failed.' }
    & $injector --dll $mod --pid $targetPid
    if ($LASTEXITCODE -ne 0) { throw "MECVR module injection failed: $mod" }
    $attached = $true
  }
  Write-Output "MECVR $Mode module attached to pid $targetPid. Close the game normally to detach."
  Wait-Process -Id $targetPid
} finally {
  if ($null -ne $game) { $game.Refresh() }
  if ($attached -and $null -ne $game -and -not $game.HasExited) {
    & $injector --shutdown $game.Id | Out-Host
  }
}
