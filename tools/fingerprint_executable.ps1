[CmdletBinding()]
param(
  [Parameter(Mandatory = $true, Position = 0)]
  [string]$Path
)

$resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
[string]$helper = @'
using System;
using System.IO;
public static class MecvrFingerprint {
  public static ulong Compute(string path, out long total) {
    const ulong offset = 14695981039346656037UL;
    const ulong prime = 1099511628211UL;
    ulong hash = offset;
    total = 0;
    byte[] buffer = new byte[64 * 1024];
    using (FileStream stream = File.OpenRead(path)) {
      int read;
      while ((read = stream.Read(buffer, 0, buffer.Length)) > 0) {
        for (int i = 0; i < read; ++i) {
          unchecked { hash = (hash ^ buffer[i]) * prime; }
        }
        total += read;
      }
    }
    return hash;
  }
}
'@
Add-Type -TypeDefinition $helper -ErrorAction Stop
[long]$total = 0
[UInt64]$hash = [MecvrFingerprint]::Compute($resolved, [ref]$total)

if ($total -eq 0) { throw "Executable is empty: $resolved" }
$fingerprint = '0x' + $hash.ToString('X16')
Write-Output "path=$resolved"
Write-Output "bytes=$total"
Write-Output "executable_fingerprint=$fingerprint"
Write-Output "# Copy this value into a reviewed native bone contract only after"
Write-Output "# resource/layout/joint evidence has been independently verified."
