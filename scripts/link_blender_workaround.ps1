# Link blender.exe around an MSVC 14.50 response-file parsing bug.
#
# CMake writes source\creator\CMakeFiles\blender.rsp as four short lines followed
# by one ~17.5 KB line. link.exe drops the first four characters of that long
# line and fails with:
#
#   LNK1181: cannot open input file 'ce\creator\CMakeFiles\blender.dir\buildinfo.c.obj'
#
# where the path should begin 'source\creator'. The file on disk is correct.
# Rewriting it one argument per line avoids the long line and links cleanly.
#
# Run this after 'cmake --build' has produced every static library, i.e. after
# the only remaining step is linking blender.exe. Then run 'cmake --install'.

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root 'build\blender'
$Rsp = Join-Path $Build 'CMakeFiles\blender.rsp'
$Multi = Join-Path $Build 'CMakeFiles\blender_multiline.rsp'

if (!(Test-Path $Rsp)) { throw "Response file not found: $Rsp. Run configure_cmake.ps1 and build first." }

. (Join-Path $PSScriptRoot 'msvc_env.ps1')

# No argument in this file is quoted, so splitting on whitespace is lossless.
# Not $args: that name is reserved for the script's own arguments.
$rspArgs = (Get-Content $Rsp -Raw) -split '\s+' | Where-Object { $_ -ne '' }
Set-Content -Path $Multi -Value ($rspArgs -join "`n") -Encoding utf8
Write-Host "[DLSS5-NR] Rewrote $($rspArgs.Count) arguments one per line."

$link = Join-Path (Split-Path -Parent $MsvcCl) 'link.exe'
$releaseDir = Join-Path $Build 'source\creator\Release'
New-Item -ItemType Directory -Force $releaseDir | Out-Null
$publicPdb = (Join-Path $releaseDir 'blender_public.pdb') -replace '\\', '/'

Push-Location $Build
# link.exe and mt.exe both write progress and warnings to stderr, which
# PowerShell 5.1 turns into terminating NativeCommandErrors under 'Stop'.
# Exit codes are checked explicitly below.
$previousErrorAction = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
  & $link /nologo "@CMakeFiles\blender_multiline.rsp" `
    /out:bin\blender.exe /implib:bin\blender.lib `
    /pdb:source\creator\Release\blender_private.pdb `
    /version:0.0 /machine:x64 /SAFESEH:NO /ignore:4099 /INCREMENTAL:NO `
    /subsystem:console /STACK:2097152 /ignore:4049 /ignore:4217 /ignore:4221 `
    /DEBUG /OPT:REF /OPT:ICF "/PDBSTRIPPED:$publicPdb"
  if ($LASTEXITCODE -ne 0) { throw "link.exe failed with exit code $LASTEXITCODE." }

  # Blender locates the DLLs in bin\blender.shared through its embedded
  # manifest. Without it the process dies at startup with 0xC0000135,
  # STATUS_DLL_NOT_FOUND, before printing anything.
  $mt = Join-Path $MsvcSdkBin 'mt.exe'
  & $mt -nologo -manifest (Join-Path $Build 'blender.exe.manifest') `
    -outputresource:"$(Join-Path $Build 'bin\blender.exe');#1"
  if ($LASTEXITCODE -ne 0) { throw "mt.exe failed with exit code $LASTEXITCODE." }
}
finally {
  $ErrorActionPreference = $previousErrorAction
  Pop-Location
}

Write-Host ""
Write-Host '[DLSS5-NR] blender.exe linked and manifest embedded.' -ForegroundColor Green
Write-Host "  $Build\bin\blender.exe"
Write-Host "  Next: cmake --install `"$Build`""
