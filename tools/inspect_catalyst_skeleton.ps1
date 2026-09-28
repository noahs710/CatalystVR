param(
  [string]$GameRoot = 'C:\Users\Gabrielle Monlea\Downloads\Mirrors Edge Catalyst',
  [string]$FrostyRoot = 'C:\Users\Gabrielle Monlea\Documents\FrostyEditor-workspace-artifact',
  [string]$RawOutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
if (-not [string]::IsNullOrWhiteSpace($RawOutputDirectory)) {
  $RawOutputDirectory = [IO.Path]::GetFullPath($RawOutputDirectory)
}
Set-Location $FrostyRoot
[Reflection.Assembly]::LoadFrom((Join-Path $FrostyRoot 'FrostySdk.dll')) | Out-Null
if (-not [FrostySdk.ProfilesLibrary]::Initialize('MirrorsEdgeCatalyst')) {
  throw 'Unable to initialize the MirrorsEdgeCatalyst profile.'
}
[FrostySdk.TypeLibrary]::Initialize($false)

$fileSystem = New-Object FrostySdk.FileSystem($GameRoot)
foreach ($source in [FrostySdk.ProfilesLibrary]::Sources) {
  if (Test-Path (Join-Path $GameRoot $source.Path)) {
    $fileSystem.AddSource($source.Path, $source.SubDirs)
  }
}
$fileSystem.Initialize([byte[]]@())
$resourceManager = New-Object FrostySdk.Managers.ResourceManager($fileSystem)
$resourceManager.Initialize()
$assetManager = New-Object FrostySdk.Managers.AssetManager($fileSystem, $resourceManager)
$assetManager.Initialize($false, $null)

foreach ($name in @('characters/skeletons/masterske_female',
                     'characters/skeletons/skeleton_female')) {
  $entry = $assetManager.GetEbxEntry($name)
  if (-not [string]::IsNullOrWhiteSpace($RawOutputDirectory)) {
    New-Item -ItemType Directory -Force $RawOutputDirectory | Out-Null
    $stream = $assetManager.GetEbxStream($entry)
    $leaf = ($entry.Name -replace '[^A-Za-z0-9_.-]', '_') + '.ebx'
    $path = Join-Path $RawOutputDirectory $leaf
    $file = [IO.File]::Create($path)
    try { $stream.CopyTo($file) } finally { $file.Dispose(); $stream.Dispose() }
    Write-Output "RAW $path"
    continue
  }
  $asset = $assetManager.GetEbx($entry)
  $root = $asset.RootObject
  Write-Output "ENTRY $($entry.Name) type=$($entry.Type) root=$($root.GetType().FullName)"
  foreach ($property in $root.GetType().GetProperties()) {
    $value = $property.GetValue($root, $null)
    if ($value -is [System.Collections.IEnumerable] -and
        $value -isnot [string]) {
      $items = @($value)
      Write-Output "  $($property.Name) type=$($property.PropertyType.FullName) count=$($items.Count)"
      for ($i = 0; $i -lt [Math]::Min($items.Count, 8); ++$i) {
        Write-Output "    [$i] $($items[$i]) ($($items[$i].GetType().FullName))"
      }
    } else {
      Write-Output "  $($property.Name) type=$($property.PropertyType.FullName) value=$value"
    }
  }
}
