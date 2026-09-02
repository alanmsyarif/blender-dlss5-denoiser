param([ValidateSet('release', 'debug')] [string]$Config = 'release')
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Source = Join-Path $Root 'blender-src'
$AdapterOut = Join-Path $Root 'build\adapter'
$BridgeSource = Join-Path $Root 'source\dlss5nr\dlss5nr_bridge.cpp'
$ShimSource = Join-Path $Root 'source\dlss5nr\caller_shim.cpp'

if (!(Test-Path (Join-Path $Source '.git'))) { throw 'Run fetch_blender.ps1 and apply_patch.ps1 first.' }
$Cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if (!$Cl) { throw 'cl.exe not found. Run from x64 Native Tools Command Prompt for Visual Studio 2022.' }
New-Item -ItemType Directory -Force $AdapterOut | Out-Null

& cl.exe /nologo /std:c++17 /EHsc /O2 /LD $ShimSource /link /OUT:"$AdapterOut\nvngx.dll_comfy.dll"
if ($LASTEXITCODE -ne 0) { throw 'Caller shim build failed.' }
& cl.exe /nologo /std:c++17 /EHsc /O2 /LD $BridgeSource d3d12.lib dxgi.lib ole32.lib /link /OUT:"$AdapterOut\dlss5nr_bridge.dll"
if ($LASTEXITCODE -ne 0) { throw 'Bridge build failed.' }

$PreviousArgs = $env:BUILD_CMAKE_ARGS
Push-Location $Source
try {
  & .\make.bat update
  if ($LASTEXITCODE -ne 0) { throw 'Blender dependency update failed.' }
  $env:BUILD_CMAKE_ARGS = '-DWITH_CYCLES_DLSS5_NR=ON'
  & .\make.bat $Config
  if ($LASTEXITCODE -ne 0) { throw 'Blender build failed.' }
}
finally {
  $env:BUILD_CMAKE_ARGS = $PreviousArgs
  Pop-Location
}
Write-Host 'Build complete. Run package_windows.ps1 after locating the generated Blender bin directory.'
