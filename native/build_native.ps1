$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    Write-Host ""
    Write-Host "[DLSS5-NR] ERROR: $Message" -ForegroundColor Red
    exit 1
}

$Root = Split-Path -Parent $PSScriptRoot
$Native = $PSScriptRoot
$Bin = Join-Path $Native 'bin'
$CallerOut = Join-Path $Root 'runtime\caller'

Write-Host "[DLSS5-NR] PowerShell native builder v0.2.0"
Write-Host "[DLSS5-NR] No Developer Command Prompt is required."

. (Join-Path $Root 'scripts\msvc_env.ps1')

New-Item -ItemType Directory -Force -Path $Bin | Out-Null
New-Item -ItemType Directory -Force -Path $CallerOut | Out-Null

function Run-Cl([string[]]$Arguments, [string]$StepName) {
    Write-Host ""
    Write-Host $StepName
    & $MsvcCl @Arguments
    if ($LASTEXITCODE -ne 0) {
        Fail "$StepName failed with cl.exe exit code $LASTEXITCODE."
    }
}

$common = @('/nologo','/std:c++17','/EHsc','/MT','/LD')
foreach ($inc in $MsvcIncludeDirs) { $common += ('/I' + $inc) }

$callerCpp = Join-Path $Root 'source\dlss5nr\caller_shim.cpp'
$callerDll = Join-Path $CallerOut 'nvngx.dll_blender.dll'
Run-Cl ($common + @('/Od', $callerCpp, '/link', ('/OUT:' + $callerDll)) + ($MsvcLibDirs | ForEach-Object { '/LIBPATH:' + $_ })) '[1/2] Building caller shim...'

$bridgeCpp = Join-Path $Root 'source\dlss5nr\dlss5nr_bridge.cpp'
$bridgeDll = Join-Path $Bin 'dlss5nr_bridge.dll'
Run-Cl ($common + @('/O2', $bridgeCpp, '/link', ('/OUT:' + $bridgeDll), 'd3d12.lib', 'dxgi.lib', 'ole32.lib') + ($MsvcLibDirs | ForEach-Object { '/LIBPATH:' + $_ })) '[2/2] Building in-process Blender bridge...'

# Remove intermediary build products created next to the invocation working directory / source.
Get-ChildItem -Path $Root -Filter '*.obj' -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
Get-ChildItem -Path $Root -Filter '*.exp' -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
Get-ChildItem -Path $Root -Filter '*.lib' -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
Get-ChildItem -Path $Native -Filter '*.obj' -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
Get-ChildItem -Path $Native -Filter '*.exp' -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
Get-ChildItem -Path $Native -Filter '*.lib' -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host '[DLSS5-NR] Build complete.' -ForegroundColor Green
Write-Host "  Bridge: $bridgeDll"
Write-Host "  Shim:   $callerDll"
Write-Host ""
Write-Host 'Place nvngx_dlssnr.dll in:'
Write-Host "  $(Join-Path $Root 'runtime\nvngx_dlssnr.dll')"
Write-Host 'then restart Blender.'
exit 0
