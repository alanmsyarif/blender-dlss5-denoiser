param(
  [ValidateSet('release', 'debug')] [string]$Config = 'release',
  # Extra -D flags appended to BUILD_CMAKE_ARGS. Use this to turn the GPU
  # backends off when CUDA and the OptiX SDK are not installed, e.g.
  #   -ExtraCMakeArgs '-DWITH_CYCLES_DEVICE_OPTIX=OFF','-DWITH_CYCLES_DEVICE_CUDA=OFF'
  [string[]]$ExtraCMakeArgs = @()
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Source = Join-Path $Root 'blender-src'
$AdapterOut = Join-Path $Root 'build\adapter'
$BridgeSource = Join-Path $Root 'source\dlss5nr\dlss5nr_bridge.cpp'
$ShimSource = Join-Path $Root 'source\dlss5nr\caller_shim.cpp'

if (!(Test-Path (Join-Path $Source '.git'))) { throw 'Run fetch_blender.ps1 and apply_patch.ps1 first.' }
# Locate MSVC and a Windows SDK, and put Visual Studio's bundled CMake on
# PATH, so this does not have to run from a Developer Command Prompt.
. (Join-Path $PSScriptRoot 'msvc_env.ps1')
if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
  throw 'cmake.exe not found. Install CMake or the Visual Studio C++ CMake tools component.'
}
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
  $CMakeArgs = @('-DWITH_CYCLES_DLSS5_NR=ON') + $ExtraCMakeArgs
  $env:BUILD_CMAKE_ARGS = ($CMakeArgs -join ' ')
  Write-Host "BUILD_CMAKE_ARGS = $env:BUILD_CMAKE_ARGS"
  & .\make.bat $Config
  if ($LASTEXITCODE -ne 0) { throw 'Blender build failed.' }
}
finally {
  $env:BUILD_CMAKE_ARGS = $PreviousArgs
  Pop-Location
}
Write-Host 'Build complete. Run package_windows.ps1 after locating the generated Blender bin directory.'
