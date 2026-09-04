# Blender 5.0.1 DLSS 5 NR experimental patchset

This repository patches Cycles in Blender `v5.0.1` to add a build-gated **DLSS 5 NR (Experimental)** denoiser for final renders and rendered viewport updates. It uses the undocumented dynamic NGX ABI approach from [ComfyUI-DLSS5-NR](https://github.com/lisitskyaa/ComfyUI-DLSS5-NR), not NVIDIA's official DLSS/Streamline SDK.

## Important limitations

- Windows x64 and NVIDIA RTX only. RTX 20 and non-RTX are not supported; Ada and Blackwell are the primary targets and Ampere is slow.
- This integration runs same-resolution and colour-only. That is a limit of this integration, not of the ABI: feature 18 also exposes `DLSSNR.Depth`, `DLSSNR.MVec` and an upscaling path. Cycles depth and motion guides are not wired up yet, so cleared guide textures are bound and the model behaves as a single-frame spatial denoiser. This is not full temporal DLSS Ray Reconstruction.
- HDR is compressed rather than discarded. The bridge's FP16 staging path applies a Reinhard curve, encodes to sRGB for the model, then inverts both on the way out, so values above 1.0 survive with reduced precision instead of being clipped. Measured against OpenImageDenoise on the same frame, the denoised result keeps 92% of the reference image energy and runs slightly conservative in the highlights, around 0.94x in the 1..15 range and 0.86x above 15. The previous hard clamp to 0..1 kept 51% and flattened 16% of the image onto exactly 1.0.
- Some resolutions hang the GPU. A 64x64 render reproducibly returns
  `DXGI_ERROR_DEVICE_HUNG` from the evaluate step, while 97x61 and 128x128 are
  fine, so it is not simply a minimum size. The bridge now detects this and
  reports it, and Cycles falls back; before the check it was silent, and the
  frame came back black while every later render failed to initialize. The
  trigger is not characterised, because narrowing it down means hanging the
  GPU repeatedly on purpose.
- Runtime compatibility is not guaranteed. Failure returns control to Cycles instead of crashing Blender, but output quality and stability require hardware testing.
- `nvngx_dlssnr.dll` and `_nvngx.dll` are proprietary and never included.

## Build

Requirements: Windows 11, MSVC with the Desktop C++ workload (Visual Studio 2022
or Build Tools 18), Git with Git LFS, CMake, and a compatible NVIDIA driver.
Building the GPU path additionally needs the **OptiX SDK 8.0 or newer** (headers
only, there is no OptiX library to link) and the **CUDA Toolkit 12.x**. Allow
roughly 40 GB of disk.

CUDA 12.x specifically: Blender pins 12.8 in `build_files/config/pipeline_config.yaml`,
and CUDA 13 fails two separate ways here, dying inside NVVM on the Cycles
megakernel and emitting PTX newer than current drivers will load.
`scripts/msvc_env.ps1` prefers an installed 12.x automatically and warns when it
can only find something newer.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\fetch_blender.ps1
powershell -ExecutionPolicy Bypass -File scripts\apply_patch.ps1
python blender-src\build_files\utils\make_update.py --no-blender

# -CudaArch names your GPU's architecture: sm_120 for Blackwell, sm_89 for Ada.
# Blender otherwise builds ten of them, each a full megakernel compile.
.\scripts\configure_cmake.ps1 -OptixRoot "C:\ProgramData\NVIDIA Corporation\OptiX SDK 9.1.0" -CudaArch sm_120

# MSVC 14.50 mis-parses the response file CMake generates for blender.exe and
# fails with LNK1181 on a path that is correct on disk. This relinks by hand.
.\scripts\link_blender_workaround.ps1
cmake --install build\blender
```

No Developer Command Prompt is needed: `scripts/msvc_env.ps1` locates MSVC, the
Windows SDK, CMake and the CUDA Toolkit itself. That is also why `make.bat` is
not used, since its Visual Studio detection does not recognise Build Tools 18.

Omitting `-CudaArch` builds the OptiX kernels without CUDA cubins. That is much
faster and works on a CUDA 13 toolkit, but it will not render: Blender's OptiX
device derives from the CUDA one and still needs the cubin.

Build the bridge and caller shim separately, into `native/bin` and
`runtime/caller`:

```powershell
.\native\build_native.ps1
```

Scripts pin Blender to commit `a3db93c5b2595a79f65f304114c23aeef8c2139f` (`v5.0.1`). `WITH_CYCLES_DLSS5_NR` defaults to `OFF`; the configure script enables it.

## Runtime setup

Follow [RUNTIME_SETUP.md](RUNTIME_SETUP.md). Then choose **DLSS 5 NR (Experimental)** under Cycles render denoising or viewport denoising. Existing OptiX and OpenImageDenoise defaults remain unchanged.

## Package

After successful build, pass directory containing `blender.exe`:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package_windows.ps1 -BlenderBin "C:\path\to\blender-build\bin\Release"
```

Packager refuses to include proprietary NVIDIA runtime DLLs.

## Verification

```bash
python -m unittest discover -s tests -v
python tools/check_no_proprietary_binaries.py
```

To measure what a denoiser does to a render rather than eyeball it, render one
fixed scene several ways. The scene is deliberately full of values above 1.0,
since that is where a display-referred model differs from an HDR one:

```powershell
$env:CYCLES_DLSS5NR_BRIDGE = "$PWD\native\bin\dlss5nr_bridge.dll"
$env:CYCLES_DLSS5NR_RUNTIME = "$PWD\runtime"
.\build\blender\bin\blender.exe -b --factory-startup -P tools\denoise_compare.py -- --out .\build\compare
```

It refuses a GPU comparison without an OptiX device, because Cycles otherwise
falls back to the CPU and off DLSS 5 NR silently, which looks indistinguishable
from a denoiser that ran and changed nothing.

Windows RTX smoke test:

1. Start patched Blender from shell containing both runtime environment variables.
2. Select Cycles with an OptiX device.
3. Select DLSS 5 NR for final render and verify Combined denoised pass.
4. Select it for viewport denoising and test camera movement and resize.
5. Remove runtime path and confirm Blender reports error without terminating.
6. Retest OptiX and OpenImageDenoise.

## Confirmed runtime behaviour

Measured against `nvngx_dlssnr.dll` 310.8.0 on an RTX 5050 (Blackwell), driver 591.86:

```text
init     SUCCESS
gpu      NVIDIA GeForce RTX 5050
abi      Init_Ext order=1 (version, info); param slots uint=3 float=6
process  SUCCESS
```

The full Cycles path is confirmed on the same machine: with an OptiX device
selected, `DLSS 5 NR (Experimental)` denoises a final render end to end. Inside
the range the model represents, its output correlates with OpenImageDenoise at
0.988, so the model itself behaves; the staging path is what bounds quality.

Three things the ABI turned out to require, none of which are guessable from the public headers:

- The snippet must be loaded **by the caller shim**, with `LoadLibraryExW` and `LOAD_WITH_ALTERED_SEARCH_PATH`. Loading it directly from the bridge deadlocks the process on `LoadLibrary`, with every thread parked in a wait state.
- `Init_Ext` takes the public NGX argument order, `(app, path, device, version, info)`.
- The capability block's float setter is at vtable slot 6, not slot 1 as the SDK header implies. Slots are probed at runtime.
- The NGX modules must never be unloaded. `FreeLibrary` on the driver's `_nvngx.dll` deadlocks the same way, so they stay resident for the process lifetime.

## References

Parameter names, the capability-block calling convention, and the caller-gate approach were recovered by [DaniilSokolyuk/video2dlssnr](https://github.com/DaniilSokolyuk/video2dlssnr). Runtime version pinning and GPU gating follow [Merserk/dlss5-visual-enhancer](https://github.com/Merserk/dlss5-visual-enhancer). Neither project is affiliated with this one.

## License

Patch integration code uses Blender-compatible SPDX headers. Adapted bridge and caller shim retain MIT attribution from ComfyUI-DLSS5-NR. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
