# Configure and build Blender directly with CMake, bypassing make.bat.
#
# make.bat autodetects Visual Studio through its own vswhere logic, which does
# not recognise Build Tools 18 and fails with "Visual Studio not found". Driving
# CMake ourselves avoids that entirely: msvc_env.ps1 already puts cl.exe, the
# Windows SDK and a CMake on PATH, which is all the Ninja generator needs.

param(
  [ValidateSet('Release', 'Debug', 'RelWithDebInfo')] [string]$Config = 'Release',
  # Blender's GPU backends need the CUDA Toolkit and the OptiX SDK. Without
  # them, -GpuOff still produces a Blender that exercises the DLSS 5 NR
  # translation unit and all of its enum plumbing.
  [switch]$GpuOff,
  # Path to the OptiX SDK root, the directory holding 'include/optix.h'. Cycles
  # asks for OptiX 8.0.0 and FindOptiX only requires the include directory, so
  # the headers alone are enough: there is no OptiX library to link.
  [string]$OptixRoot,
  # Architectures to compile Cycles GPU kernels for, e.g. 'sm_120' for
  # Blackwell. Needs the CUDA Toolkit on PATH for nvcc. Leaving this unset
  # builds an OptiX-capable Blender with no precompiled kernels, which is
  # enough to enumerate the device but not to render on it.
  [string]$CudaArch,
  [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Source = Join-Path $Root 'blender-src'
$Build = Join-Path $Root 'build\blender'

if (!(Test-Path (Join-Path $Source 'CMakeLists.txt'))) { throw 'Run fetch_blender.ps1 and apply_patch.ps1 first.' }
if (!(Test-Path (Join-Path $Source 'lib\windows_x64'))) { throw 'Libraries missing. Run: python build_files\utils\make_update.py --no-blender' }

. (Join-Path $PSScriptRoot 'msvc_env.ps1')

$cmakeArgs = @(
  '-S', $Source,
  '-B', $Build,
  '-G', 'Ninja',
  "-DCMAKE_BUILD_TYPE=$Config",
  '-DCMAKE_C_COMPILER=cl',
  '-DCMAKE_CXX_COMPILER=cl',
  '-DWITH_CYCLES_DLSS5_NR=ON',
  # Blender only picks the portable install path when CMake reports the prefix
  # as freshly defaulted (CMakeLists.txt:1262). On a reconfigure of an existing
  # cache that flag is not set, so the prefix silently falls back to
  # 'C:/Program Files (x86)/Blender' and 'cmake --install' dies with
  # "file cannot create directory ... Maybe need administrative privileges".
  # Passing it every time makes the install target independent of cache state.
  "-DCMAKE_INSTALL_PREFIX=$Build/bin",
  # CMake 4.2 leaves CMAKE_<LANG>_FLAGS_RELEASE empty, and Blender's
  # platform_win32.cmake only does string(APPEND ...) on it, assuming CMake
  # already seeded "/O2 /Ob2 /DNDEBUG". The result is a Release build with
  # NDEBUG defined but no optimisation, which is both slow and fails to link:
  # editmesh_undo.cc calls BM_mesh_is_valid behind a runtime "if (false)", and
  # that function only exists when NDEBUG is undefined, so without the
  # optimiser removing the dead branch you get LNK2019.
  '-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG',
  '-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG'
)

# KNOWN ISSUE, MSVC 14.50 / Build Tools 18: link.exe mis-parses the response
# file CMake generates for blender.exe. The file holds four short lines followed
# by one ~17.5 KB line, and the linker drops the first four characters of that
# long line, failing with
#   LNK1181: cannot open input file 'ce\creator\...uildinfo.c.obj'
# where the path should start with 'source\creator'. The file on disk is
# correct; only the linker's reading of it is wrong.
#
# Turning response files off entirely is not a fix either: the objects stay in a
# response file while the libraries move inline, and the result exceeds the
# 32 KB command line limit. Until this is resolved, link blender.exe by hand
# with a response file rewritten one argument per line, then embed the manifest
# with mt.exe. See scripts/link_blender_workaround.ps1.

if ($OptixRoot) {
  if (!(Test-Path (Join-Path $OptixRoot 'include\optix.h'))) {
    throw "No include\optix.h under $OptixRoot. Point -OptixRoot at the SDK root."
  }
  $cmakeArgs += @(
    '-DWITH_CYCLES_DEVICE_OPTIX=ON',
    '-DWITH_CYCLES_DEVICE_CUDA=ON',
    "-DOPTIX_ROOT_DIR=$OptixRoot"
  )
  if ($CudaArch) {
    # Blender defaults to ten architectures, each a full megakernel nvcc run.
    # Building only the one this machine uses turns hours into minutes. Note
    # CUDA 13 dropped Maxwell and Pascal, so the default list fails outright
    # there; naming an architecture avoids that too.
    $cmakeArgs += @(
      '-DWITH_CYCLES_CUDA_BINARIES=ON',
      "-DCYCLES_CUDA_BINARIES_ARCH=$CudaArch"
    )
  }
}

if ($GpuOff) {
  $cmakeArgs += @(
    '-DWITH_CYCLES_DEVICE_OPTIX=OFF',
    '-DWITH_CYCLES_DEVICE_CUDA=OFF',
    '-DWITH_CYCLES_CUDA_BINARIES=OFF',
    '-DWITH_CYCLES_DEVICE_HIP=OFF',
    '-DWITH_CYCLES_DEVICE_ONEAPI=OFF'
  )
}

Write-Host ""
Write-Host "[DLSS5-NR] Configuring: $Build"
Invoke-Native cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE." }

if ($ConfigureOnly) {
  Write-Host "[DLSS5-NR] Configure complete. Build with: cmake --build `"$Build`""
  exit 0
}

Write-Host ""
Write-Host "[DLSS5-NR] Building..."
Invoke-Native cmake --build $Build --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }

Write-Host ""
Write-Host '[DLSS5-NR] Build complete.' -ForegroundColor Green
Write-Host "  Binaries: $Build\bin"
