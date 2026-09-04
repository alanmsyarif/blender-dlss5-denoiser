# Locate an MSVC x64 toolchain and a Windows SDK, then put them on PATH.
#
# Dot-source this. It sets $env:PATH, $env:INCLUDE and $env:LIB, and defines
# $MsvcCl, $MsvcIncludeDirs and $MsvcLibDirs in the caller's scope, so neither
# build script needs a Developer Command Prompt.

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    Write-Host ""
    Write-Host "[DLSS5-NR] ERROR: $Message" -ForegroundColor Red
    exit 1
}

$pf86 = [Environment]::GetFolderPath('ProgramFilesX86')
$pf = [Environment]::GetFolderPath('ProgramFiles')
$programRoots = @($pf86, $pf) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) -and (Test-Path $_) } | Select-Object -Unique
if (-not $programRoots -or $programRoots.Count -eq 0) { Fail 'Could not determine Program Files directories.' }

# Find the newest installed x64 MSVC compiler. Supports Build Tools and
# full Visual Studio installations on local machines and GitHub Actions.
$clCandidates = @()
foreach ($programRoot in $programRoots) {
    $vsRoot = Join-Path $programRoot 'Microsoft Visual Studio'
    if (-not (Test-Path $vsRoot)) { continue }
    $clCandidates += Get-ChildItem -Path $vsRoot -Filter cl.exe -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\VC\\Tools\\MSVC\\[^\\]+\\bin\\Hostx64\\x64\\cl\.exe$' } |
        Select-Object -ExpandProperty FullName
}
$clCandidates = $clCandidates | Select-Object -Unique
if (-not $clCandidates -or $clCandidates.Count -eq 0) {
    Fail 'MSVC x64 compiler cl.exe was not found. Install Desktop development with C++.'
}

# Sort by the toolset version directory where possible.
$MsvcCl = $clCandidates | Sort-Object {
    if ($_ -match '\\MSVC\\([^\\]+)\\bin\\Hostx64') {
        try { [version]$Matches[1] } catch { [version]'0.0' }
    } else { [version]'0.0' }
} -Descending | Select-Object -First 1

$msvcVersionDir = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MsvcCl)))
# cl = ...\MSVC\14.xx\bin\Hostx64\x64\cl.exe; four parents => ...\MSVC\14.xx
if (-not (Test-Path (Join-Path $msvcVersionDir 'include'))) {
    # Defensive fallback derived from regex.
    if ($MsvcCl -match '^(.*\\MSVC\\[^\\]+)\\bin\\Hostx64\\x64\\cl\.exe$') {
        $msvcVersionDir = $Matches[1]
    }
}
$msvcInclude = Join-Path $msvcVersionDir 'include'
$msvcLib = Join-Path $msvcVersionDir 'lib\x64'
if (-not (Test-Path (Join-Path $msvcInclude 'vector'))) { Fail "MSVC include directory is invalid: $msvcInclude" }
if (-not (Test-Path $msvcLib)) { Fail "MSVC library directory is invalid: $msvcLib" }

# Find the newest Windows 10/11 SDK installed in Windows Kits\10.
$sdkRoot = $null
foreach ($programRoot in $programRoots) {
    $candidateSdk = Join-Path $programRoot 'Windows Kits\10'
    if (Test-Path (Join-Path $candidateSdk 'Include')) { $sdkRoot = $candidateSdk; break }
}
if (-not $sdkRoot) { Fail 'Windows Kits\10 was not found.' }
$sdkIncludeRoot = Join-Path $sdkRoot 'Include'
$sdkLibRoot = Join-Path $sdkRoot 'Lib'
if (-not (Test-Path $sdkIncludeRoot)) { Fail "Windows SDK Include directory not found: $sdkIncludeRoot" }

$sdkVersions = Get-ChildItem -Path $sdkIncludeRoot -Directory -ErrorAction SilentlyContinue |
    Where-Object {
        (Test-Path (Join-Path $_.FullName 'um\windows.h')) -and
        (Test-Path (Join-Path $sdkLibRoot ($_.Name + '\um\x64\d3d12.lib')))
    } |
    Sort-Object {
        try { [version]$_.Name } catch { [version]'0.0' }
    } -Descending

if (-not $sdkVersions -or $sdkVersions.Count -eq 0) {
    Fail 'A usable Windows SDK with windows.h and d3d12.lib was not found.'
}
$sdkVersion = $sdkVersions[0].Name
$sdkIncBase = Join-Path $sdkIncludeRoot $sdkVersion
$sdkLibBase = Join-Path $sdkLibRoot $sdkVersion

$MsvcIncludeDirs = @(
    $msvcInclude,
    (Join-Path $sdkIncBase 'ucrt'),
    (Join-Path $sdkIncBase 'shared'),
    (Join-Path $sdkIncBase 'um'),
    (Join-Path $sdkIncBase 'winrt'),
    (Join-Path $sdkIncBase 'cppwinrt')
) | Where-Object { Test-Path $_ }

$MsvcLibDirs = @(
    $msvcLib,
    (Join-Path $sdkLibBase 'ucrt\x64'),
    (Join-Path $sdkLibBase 'um\x64')
) | Where-Object { Test-Path $_ }

$clDir = Split-Path -Parent $MsvcCl
$env:PATH = "$clDir;$env:PATH"
$env:INCLUDE = ($MsvcIncludeDirs -join ';')
$env:LIB = ($MsvcLibDirs -join ';')

# The SDK's tool directory holds rc.exe and mt.exe. Compiling and linking works
# without them, but anything that builds a resource or a manifest - which
# includes CMake's own compiler probe - fails with "RC Pass 1 ... no such file
# or directory" and CMAKE_MT-NOTFOUND.
$MsvcSdkBin = Join-Path $sdkRoot ('bin\' + $sdkVersion + '\x64')
if (Test-Path (Join-Path $MsvcSdkBin 'rc.exe')) {
    $env:PATH = "$MsvcSdkBin;$env:PATH"
}
else {
    Write-Host "[DLSS5-NR] WARNING: rc.exe not found in $MsvcSdkBin" -ForegroundColor Yellow
}

# Visual Studio ships a CMake that is not on PATH by default. Blender's
# make.bat needs one, so fall back to it rather than demanding a separate
# CMake install.
if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    $vsInstallRoot = $MsvcCl -replace '\\VC\\Tools\\MSVC\\.*$', ''
    $bundledCMake = Join-Path $vsInstallRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
    if (Test-Path (Join-Path $bundledCMake 'cmake.exe')) {
        $env:PATH = "$bundledCMake;$env:PATH"
    }
}

Write-Host "[DLSS5-NR] Compiler:    $MsvcCl"
Write-Host "[DLSS5-NR] MSVC root:   $msvcVersionDir"
Write-Host "[DLSS5-NR] Windows SDK: $sdkVersion"
$cmakeCmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($cmakeCmd) { Write-Host "[DLSS5-NR] CMake:       $($cmakeCmd.Source)" }
