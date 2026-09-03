$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Dist = Join-Path $Root 'dist'
$Stage = Join-Path $env:TEMP 'blender-dlss5-nr-package'
$Zip = Join-Path $Dist 'dlss5_nr-0.1.0-windows-x64.zip'

$Required = @(
    'blender_manifest.toml', '__init__.py', 'native_bridge.py', 'operators.py',
    'pixels.py', 'properties.py', 'ui.py', 'LICENSE', 'README.md',
    'THIRD_PARTY_NOTICES.md', 'native\bin\dlss5nr_bridge.dll',
    'runtime\caller\nvngx.dll_blender.dll'
)
foreach ($Relative in $Required) {
    if (-not (Test-Path (Join-Path $Root $Relative))) {
        throw "Missing required package file: $Relative"
    }
}

if (Test-Path (Join-Path $Root 'runtime\nvngx_dlssnr.dll')) {
    Write-Warning 'nvngx_dlssnr.dll is proprietary and will not be packaged.'
}

Remove-Item $Stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $Stage | Out-Null
foreach ($Relative in $Required) {
    $Source = Join-Path $Root $Relative
    $Target = Join-Path $Stage $Relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $Target) -Force | Out-Null
    Copy-Item $Source $Target
}
Copy-Item (Join-Path $Root 'runtime\README.txt') (Join-Path $Stage 'runtime\README.txt')

New-Item -ItemType Directory -Path $Dist -Force | Out-Null
Remove-Item $Zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $Stage '*') -DestinationPath $Zip -CompressionLevel Optimal
Remove-Item $Stage -Recurse -Force
Write-Host "Created $Zip"
Write-Host 'Install ZIP, then place nvngx_dlssnr.dll in installed extension runtime directory.'
