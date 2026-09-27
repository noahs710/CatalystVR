[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)] [string]$GameRoot,
  [string]$OutputPath = '.\catalyst_asset_index.json',
  [int]$MaxFileMiB = 64
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $GameRoot).Path
$trees = @('Data\Win32', 'Patch\Win32')
$files = foreach ($tree in $trees) {
  $dir = Join-Path $root $tree
  if (-not (Test-Path -LiteralPath $dir)) { continue }
  Get-ChildItem -LiteralPath $dir -File -Include *.toc, *.sb |
    ForEach-Object {
      [pscustomobject]@{
        Path = $_.FullName
        Layer = ($tree -split '\\')[0]
        RelativePath = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
        Length = $_.Length
      }
    }
}

function Get-AsciiRuns([string]$Path, [int]$MaxBytes) {
  $stream = [System.IO.File]::OpenRead($Path)
  try {
    $buffer = New-Object byte[] 1048576
    $run = New-Object System.Text.StringBuilder
    $remaining = [Math]::Min([int64]$MaxBytes, $stream.Length)
    while ($remaining -gt 0) {
      $want = [int][Math]::Min($buffer.Length, $remaining)
      $read = $stream.Read($buffer, 0, $want)
      if ($read -le 0) { break }
      for ($i = 0; $i -lt $read; ++$i) {
        $b = $buffer[$i]
        if (($b -ge 32 -and $b -le 126) -or $b -eq 9) {
          [void]$run.Append([char]$b)
        } else {
          if ($run.Length -ge 4) { $run.ToString() }
          [void]$run.Clear()
        }
      }
      $remaining -= $read
    }
    if ($run.Length -ge 4) { $run.ToString() }
  } finally {
    $stream.Dispose()
  }
}

$packagePattern = '(?i)(?:^|[^a-z0-9_])((?:characters(?:_[a-z0-9]+)?|character_customization|animations?|skeletons?|skinning|rig|ik)(?:/[a-z0-9_.-]+)+)'
$evidencePattern = '(?i)(skeleton|master.?skeleton|skinned|animation|pose|twoboneik|jointmapping|effector|morph|character)'
$rows = [System.Collections.Generic.List[object]]::new()
$maxBytes = [int64]$MaxFileMiB * 1MB

foreach ($file in $files) {
  if ($file.Length -gt $maxBytes) {
    Write-Verbose "Skipping oversized archive: $($file.RelativePath)"
    continue
  }
  $seen = @{}
  foreach ($run in (Get-AsciiRuns -Path $file.Path -MaxBytes $maxBytes)) {
    foreach ($match in [regex]::Matches($run, $packagePattern)) {
      $value = $match.Groups[1].Value.Trim('/').ToLowerInvariant()
      if (-not $seen.ContainsKey($value)) {
        $seen[$value] = $true
        $category = if ($value -match '(?i)character') { 'character-package' }
          elseif ($value -match '(?i)skeleton|skinning|rig|joint|ik') { 'skeleton-or-ik' }
          else { 'animation' }
        $rows.Add([pscustomobject]@{
          Layer = $file.Layer
          RelativePath = $file.RelativePath
          ArchiveBytes = $file.Length
          Category = $category
          Package = $value
          Evidence = $value
        })
      }
    }
    foreach ($match in [regex]::Matches($run, $evidencePattern)) {
      $value = $match.Value.ToLowerInvariant()
      if ($value -in @('animation', 'character')) { continue }
      $rows.Add([pscustomobject]@{
        Layer = $file.Layer
        RelativePath = $file.RelativePath
        ArchiveBytes = $file.Length
        Category = 'runtime-type-evidence'
        Package = $null
        Evidence = $value
      })
    }
  }
}

$deduped = $rows | Sort-Object Layer, RelativePath, Category, Package, Evidence -Unique
$summary = [pscustomobject]@{
  Schema = 'mecvr.catalyst.asset-index.v1'
  GeneratedUtc = [DateTime]::UtcNow.ToString('o')
  GameRoot = $root
  Layers = @('Data', 'Patch')
  ArchiveCount = @($files).Count
  SkippedOversizedArchives = @($files | Where-Object { $_.Length -gt $maxBytes }).Count
  Records = @($deduped)
}
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $resolvedOutput -Encoding UTF8
Write-Output "Indexed $($summary.ArchiveCount) archives; wrote $($deduped.Count) records to $resolvedOutput"
