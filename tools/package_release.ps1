[CmdletBinding()]
param(
  [string]$BuildDir,
  [string]$OutputDir,
  [string]$Version = '1.5.2.10-alpha'
)

$ErrorActionPreference = 'Stop'
$scriptRoot = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = Join-Path $scriptRoot '..\build' }
if ([string]::IsNullOrWhiteSpace($OutputDir)) { $OutputDir = Join-Path $scriptRoot '..\dist' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = (Resolve-Path $BuildDir).Path
$stage = Join-Path $OutputDir "MECVR-$Version"
$stageResolved = [IO.Path]::GetFullPath($stage)
$distResolved = [IO.Path]::GetFullPath((Resolve-Path (New-Item -ItemType Directory -Force $OutputDir)).Path)
if (-not $stageResolved.StartsWith($distResolved + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
  throw "Refusing to package outside the dist directory: $stageResolved"
}

if (Test-Path -LiteralPath $stageResolved) { Remove-Item -LiteralPath $stageResolved -Recurse -Force }
New-Item -ItemType Directory -Force $stageResolved | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stageResolved 'bin') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stageResolved 'docs') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stageResolved 'licenses') | Out-Null

Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $stageResolved
Copy-Item -LiteralPath (Join-Path $repo 'THIRD-PARTY-NOTICES.txt') -Destination $stageResolved
Copy-Item -LiteralPath (Join-Path $repo 'third_party\openxr\LICENSE') `
  -Destination (Join-Path $stageResolved 'licenses\OPENXR-LICENSE.txt')

$required = @(
  'src\Release\mecvr_inject.exe',
  'src\Release\mecvr_runtime.dll',
  'src\Release\mecvr_m3b_live.dll',
  'src\Release\mecvr_m2b_live.dll',
  'src\Release\openxr_loader.dll',
  'src\Release\mecvr_launcher.exe'
)
foreach ($relative in $required) {
  $source = Join-Path $build $relative
  if (-not (Test-Path -LiteralPath $source)) { throw "Missing build artifact: $source" }
  Copy-Item -LiteralPath $source -Destination (Join-Path $stageResolved 'bin')
}

@('README.md', 'RELEASE.md', 'CAMERA.md', 'INPUT.md', 'TEST_MATRIX.md', 'ENGINE_INTEL.md', 'LAUNCHER.md', 'FEATURE_BENCHMARK.md', 'FROSTY_COEXISTENCE.md', 'RETAIL_EVIDENCE.md') | ForEach-Object {
  $source = Join-Path $repo "docs\$_"
  if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $stageResolved 'docs') }
}
Copy-Item -LiteralPath (Join-Path $repo 'tools\launch_preview.ps1') -Destination $stageResolved
Copy-Item -LiteralPath (Join-Path $repo 'tools\index_catalyst_assets.ps1') -Destination $stageResolved
Copy-Item -LiteralPath (Join-Path $repo 'tools\fingerprint_executable.ps1') -Destination $stageResolved
Copy-Item -LiteralPath (Join-Path $build 'src\Release\mecvr_launcher.exe') -Destination $stageResolved

$testOutput = & ctest --test-dir $build -C Release --output-on-failure 2>&1
$testOutput | Set-Content -LiteralPath (Join-Path $stageResolved 'ctest-release.txt') -Encoding UTF8
if ($LASTEXITCODE -ne 0) { throw 'CTest failed; refusing to package a red release.' }
$launcherTest = & (Join-Path $stageResolved 'mecvr_launcher.exe') --self-test 2>&1
$launcherExit = $LASTEXITCODE
@('Launcher self-test: PASS', "Exit code: $launcherExit") + @($launcherTest) |
  Set-Content -LiteralPath (Join-Path $stageResolved 'launcher-self-test.txt') -Encoding UTF8
if ($launcherExit -ne 0) { throw 'Launcher self-test failed; refusing to package a broken release.' }

$manifest = @(
  "MECVR version: $Version"
  "Built UTC: $([DateTime]::UtcNow.ToString('o'))"
  'Artifact: preview foundation; not a complete retail game conversion.'
  'Stereo: the public launcher enables correctly converged immersive projection with a square-eye crop contract, per-eye camera poses, and temporal per-eye capture; simultaneous native dual-pass remains a future optimization.'
  'Transport: capability-gated D3D11 shared-texture GPU path is default; CPU readback is the automatic fallback.'
  'Camera: opt-in M3b bridge; requires an OpenXR runtime and explicit calibration.'
  'Launcher: mecvr_launcher.exe provides persisted settings, performance profiles, dry-run checks, and launch handoff.'
  'Licensing: LICENSE, THIRD-PARTY-NOTICES.txt, and licenses/OPENXR-LICENSE.txt are included.'
  'Frosty: optional explicit backend launches a configured pack and injects only into a newly matched exact Catalyst executable.'
  'IK: deterministic 21-joint full-body procedural pose layer is included and headless-tested.'
  'Asset index: tools/index_catalyst_assets.ps1 performs a read-only Data/Patch character package scan.'
  'Skeleton: the retail 169-bone female hierarchy and arm indices are asset-hash verified; game-visible writes still require a matching live palette/layout signature.'
  'Tests: see ctest-release.txt; packaging stops on any failure.'
)
$manifest | Set-Content -LiteralPath (Join-Path $stageResolved 'MANIFEST.txt') -Encoding UTF8
$archive = Join-Path $distResolved "MECVR-$Version.zip"
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
Compress-Archive -Path (Join-Path $stageResolved '*') -DestinationPath $archive -CompressionLevel Optimal
Write-Output "Packaged $stageResolved"
Write-Output "Archive $archive"
