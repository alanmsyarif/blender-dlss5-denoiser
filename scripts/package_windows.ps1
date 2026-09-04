param([Parameter(Mandatory=$true)] [string]$BlenderBin)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Adapter = Join-Path $Root 'build\adapter'
$Stage = Join-Path $Root 'build\package\blender-5.0.1-dlss5nr-experimental'
$Dist = Join-Path $Root 'dist'
$Archive = Join-Path $Dist 'blender-5.0.1-dlss5nr-experimental-windows-x64.zip'

if (!(Test-Path (Join-Path $BlenderBin 'blender.exe'))) { throw "blender.exe not found in $BlenderBin" }
foreach ($File in @('dlss5nr_bridge.dll', 'nvngx.dll_comfy.dll')) {
  if (!(Test-Path (Join-Path $Adapter $File))) { throw "Missing adapter build: $File" }
}
Remove-Item $Stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $Stage | Out-Null
Copy-Item (Join-Path $BlenderBin '*') $Stage -Recurse
Copy-Item (Join-Path $Adapter 'dlss5nr_bridge.dll') $Stage
$Runtime = Join-Path $Stage 'dlss5nr_runtime\caller'
New-Item -ItemType Directory -Force $Runtime | Out-Null
Copy-Item (Join-Path $Adapter 'nvngx.dll_comfy.dll') $Runtime
Copy-Item (Join-Path $Root 'RUNTIME_SETUP.md') $Stage

# Matching on filename alone missed a renamed copy, and this archive is meant
# to be handed to other people, so a miss here redistributes NVIDIA's runtime
# rather than merely committing it. ProductName is checked too: RUNTIME_SETUP.md
# pins the tested nvngx_dlssnr.dll as product name 'NVIDIA DLSSNR', which no
# file we ship carries. _nvngx.dll is the driver's own loader with a different
# product name, so the name list stays as well.
$Forbidden = Get-ChildItem $Stage -Recurse -File | Where-Object {
  $_.Name -in @('nvngx_dlssnr.dll', '_nvngx.dll') -or $_.VersionInfo.ProductName -eq 'NVIDIA DLSSNR'
}
if ($Forbidden) { throw 'Refusing to package proprietary NVIDIA runtime DLLs.' }
New-Item -ItemType Directory -Force $Dist | Out-Null
Remove-Item $Archive -Force -ErrorAction SilentlyContinue
Compress-Archive -Path "$Stage\*" -DestinationPath $Archive -CompressionLevel Optimal
Get-FileHash $Archive -Algorithm SHA256 | Format-List
