# Blender 5.0.1 DLSS 5 NR experimental patchset

This repository patches Cycles in Blender `v5.0.1` to add a build-gated **DLSS 5 NR (Experimental)** denoiser for final renders and rendered viewport updates. It uses the undocumented dynamic NGX ABI approach from [ComfyUI-DLSS5-NR](https://github.com/lisitskyaa/ComfyUI-DLSS5-NR), not NVIDIA's official DLSS/Streamline SDK.

## Important limitations

- Windows x64 and NVIDIA RTX only. RTX 20 and non-RTX are not supported; Ada and Blackwell are the primary targets and Ampere is slow.
- This integration runs same-resolution and colour-only. That is a limit of this integration, not of the ABI: feature 18 also exposes `DLSSNR.Depth`, `DLSSNR.MVec` and an upscaling path. Cycles depth and motion guides are not wired up yet, so cleared guide textures are bound and the model behaves as a single-frame spatial denoiser. This is not full temporal DLSS Ray Reconstruction.
- HDR values are constrained by the bridge's normalized FP16 staging path: colour is clamped to 0..1, encoded to sRGB for the model, and decoded back to linear. Anything above 1.0 in the render is crushed before denoising.
- Runtime compatibility is not guaranteed. Failure returns control to Cycles instead of crashing Blender, but output quality and stability require hardware testing.
- `nvngx_dlssnr.dll` and `_nvngx.dll` are proprietary and never included.

## Build

Requirements: Windows 11, Visual Studio 2022 with Desktop C++ workload, Git, Subversion, CMake, compatible NVIDIA driver, and enough disk space for Blender build.

Run from **x64 Native Tools Command Prompt for VS 2022**:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\fetch_blender.ps1
powershell -ExecutionPolicy Bypass -File scripts\apply_patch.ps1
powershell -ExecutionPolicy Bypass -File scripts\build_windows.ps1
```

Scripts pin Blender to commit `a3db93c5b2595a79f65f304114c23aeef8c2139f` (`v5.0.1`). `WITH_CYCLES_DLSS5_NR` defaults to `OFF`; build script enables it.

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

Three things the ABI turned out to require, none of which are guessable from the public headers:

- The snippet must be loaded **by the caller shim**, with `LoadLibraryExW` and `LOAD_WITH_ALTERED_SEARCH_PATH`. Loading it directly from the bridge deadlocks the process on `LoadLibrary`, with every thread parked in a wait state.
- `Init_Ext` takes the public NGX argument order, `(app, path, device, version, info)`.
- The capability block's float setter is at vtable slot 6, not slot 1 as the SDK header implies. Slots are probed at runtime.
- The NGX modules must never be unloaded. `FreeLibrary` on the driver's `_nvngx.dll` deadlocks the same way, so they stay resident for the process lifetime.

## References

Parameter names, the capability-block calling convention, and the caller-gate approach were recovered by [DaniilSokolyuk/video2dlssnr](https://github.com/DaniilSokolyuk/video2dlssnr). Runtime version pinning and GPU gating follow [Merserk/dlss5-visual-enhancer](https://github.com/Merserk/dlss5-visual-enhancer). Neither project is affiliated with this one.

## License

Patch integration code uses Blender-compatible SPDX headers. Adapted bridge and caller shim retain MIT attribution from ComfyUI-DLSS5-NR. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
