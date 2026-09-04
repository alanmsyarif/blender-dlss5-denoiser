# Blender 5.0.1 DLSS 5 NR experimental patchset

This repository patches Cycles in Blender `v5.0.1` to add a build-gated **DLSS 5 NR (Experimental)** denoiser for final renders and rendered viewport updates. It uses the undocumented dynamic NGX ABI approach from [ComfyUI-DLSS5-NR](https://github.com/lisitskyaa/ComfyUI-DLSS5-NR), not NVIDIA's official DLSS/Streamline SDK.

## Status: this targets the wrong runtime

This project drives `nvngx_dlssnr.dll`, NGX feature 18, over a D3D12 bridge with
CPU staging and a Reinhard tonemap. It works, it is measured, and it is the wrong
choice for a Cycles denoiser. Anyone continuing this should read this section
before writing code.

Feature 18 is an appearance model. Its 61 parameters cover Style, Intensity,
Tone, Structure, Skin, AutoMask and UI compositing. It has no albedo, normal,
roughness or hit distance inputs, which is to say none of the guides a denoiser
needs. Measured on `build/blender/tests/human_test.blend` at 16 samples against a
512 sample reference:

```text
OptiX          3.46% RMSE
undenoised     9.19%
this project  25.13%   keeping 85% of reference highlights
```

It moves the image away from the converged result. That is not a tuning problem.

The right target is `nvngx_dlssd.dll`, product name "NVIDIA DLSS Ray
Reconstruction", driven through the **CUDA** NGX entry points
(`NVSDK_NGX_CUDA_Init_Ext1`, `CreateFeature1`, `AllocateParameters`,
`EvaluateFeature`, `ReleaseFeature`, `Shutdown1`). That runs on Cycles' own
device buffers: no D3D12 device, no CPU round trip, no tonemapping, no FP16
staging. Its parameters are what a path tracer denoiser actually wants, including
`DiffuseHitDistance`, `SpecularHitDistance`, diffuse and specular ray directions,
`ReflectedAlbedo` and a `ScreenSpaceSubsurfaceScatteringGuide`, on top of the
albedo, normal and roughness guides Cycles already produces for OptiX and
OpenImageDenoise.

A working build proving this exists and denoises the viewport in real time
without jitter.

Most of the Cycles side here survives such a rewrite: the denoiser class and its
registration, the UI properties, the pass plumbing, and the camera reprojection
that derives motion vectors between evaluations. What deletes is most of
`source/dlss5nr/dlss5nr_bridge.cpp`.

The D3D12 approach was inherited from ComfyUI-DLSS5-NR, an image processing tool
where it is the sensible design. Every defect fixed in this repository's history,
highlight crushing, the tonemap knee, zero motion vectors and the per frame CPU
round trip, is downstream of carrying that design into a renderer.

## Important limitations

- Windows x64 and NVIDIA RTX only. RTX 20 and non-RTX are not supported; Ada and Blackwell are the primary targets and Ampere is slow.
- Temporal accumulation needs the Z and Vector passes. When the view layer
  carries both, Cycles' depth and motion are uploaded as `DLSSNR.Depth` and
  `DLSSNR.MVec` and the model accumulates across evaluations, which is what
  Ray Reconstruction is built around; the console says which mode is in use.
  Without them the guides stay cleared and every evaluation resets, because
  accumulating against absent correspondence produced a 17% swing in output
  energy between identical renders. Measured on a fixed scene, real guides
  take the spread from 2.5% to 1.1%.
- Still same-resolution. `DLSSNR.ScalingRatio` is the runtime's upscaling
  control and is left unset, and Cycles has no upscaling stage for a denoiser
  to feed regardless: a denoiser writes back into the buffer it was given.
- HDR is compressed rather than discarded. The bridge's FP16 staging path applies a Reinhard curve, encodes to sRGB for the model, then inverts both on the way out, so values above 1.0 survive with reduced precision instead of being clipped. Measured against OpenImageDenoise on the same frame, the denoised result keeps 92% of the reference image energy and runs slightly conservative in the highlights, around 0.94x in the 1..15 range and 0.86x above 15. The previous hard clamp to 0..1 kept 51% and flattened 16% of the image onto exactly 1.0.
- Frames with a short longer side are refused. A 64x64 evaluate hangs the GPU
  outright, returning `DXGI_ERROR_DEVICE_HUNG`, and the device never comes back,
  so every later render in that process fails too. 128x32 has the same 4096
  pixels and denoises correctly, so the constraint is the longer side rather
  than the area; 96x96 and 97x61 both work. The bridge refuses anything with a
  longer side below 96 and Cycles carries on. Between 65 and 95 is untested,
  since each probe means hanging the GPU again.
- Most appearance controls the ABI exposes are not reachable from Cycles. The
  bridge carries `DLSSNR.Style`, `LocalToneStrength`, `LocalStructureStrength`,
  `SkinStructureStrength` and `UseAutoMask`, and the addon exposes all of them,
  but the Cycles denoiser hardcodes every one to a default. Measured against a
  fixed frame, Style moves the image 41 to 54%, Structure up to 66%, Tone 27%
  and the automatic mask 12%, so these are not subtle. `SkinStructureStrength`
  does nothing on its own and only takes effect with `UseAutoMask` enabled,
  which is what makes it a material specific control rather than a global one.
  `Hint.Render.Preset` and `Intensity` measured as exactly no change, so either
  those names are wrong or the runtime ignores them.
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

Viewport denoising is not reachable from a background render, and every bug this
project has hit in the denoiser's lifetime showed up only under the viewport's
calling pattern: repeated invocations, view changes, and resolution changes
mid-session. This drives a real rendered viewport and quits by itself, so run it
without `-b`:

```powershell
.\build\blender\bin\blender.exe --factory-startup --debug-cycles -P tools\viewport_check.py
```

A clean run ends with `VIEWPORT_CHECK_DONE ... failures=0`. Also grep the output
for `EXCEPTION_ACCESS_VIOLATION`, `DEVICE_HUNG` and `Could not create a D3D12
device`: those are the three ways this has broken before, and none of them stop
the session on their own. What it cannot judge is whether the result looks
stable, since with cleared depth and motion guides the model denoises each frame
independently and any flicker has to be seen.

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

## What feature 18 actually exposes

Scanning `nvngx_dlssnr.dll` 310.8.0 for its own parameter strings gives the
complete surface: 61 names, and nothing outside them will be read.

Inputs and outputs: `Color`, `Depth`, `MVec`, `Output`, `Backbuffer`, `UI`,
`UIAlpha`, `ControlMask`, `BidirectionalDistortionField`, each with
`...SubrectBaseX/BaseY/Width/Height`.

Controls: `Enabled`, `Reset`, `Width`, `Height`, `ScalingRatio`,
`DepthInverted`, `MVecScaleX`, `MVecScaleY`, `Style`, `Hint.Render.Preset`,
`Intensity`, `LocalToneStrength`, `LocalStructureStrength`,
`SkinStructureStrength`, `UseAutoMask`, `UICorrection`.

There is no separately named parameter for lighting, semantics, hair or fabric.
Those are not switches; they are what the model does with the frame, steered by
the controls above. See "Generative neural rendering" below.

Names this project sets that the runtime does not contain, and which therefore
did nothing: `OutWidth`, `OutHeight`, `DLSSNR.InputWidth`, `DLSSNR.InputHeight`,
`DLSSNR.OutputWidth`, `DLSSNR.OutputHeight`, `DLSSNR.Upscaling`. They have been
removed. `DLSSNR.Upscaling` in particular made the bridge look like it was
choosing same-resolution operation when it was only failing to ask for anything.

Cross-checked against the two projects that inject this runtime into games,
[DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) and
[1-Click-DLSS5](https://github.com/reiluisii/1-Click-DLSS5). Both drive the same
feature 18 and both do what this project does: hand it colour, depth and motion
vectors and take a reconstructed frame back. Neither implements generative
lighting, indirect or bounced light, or material simulation either.

Their settings names are their own, not NVIDIA's. `NeuralUplift`, `NRStyle`,
`NRGlobalTone`, `NREnableUpscaling`, `NRToggleKey` and `EnableHooks` appear
nowhere in `nvngx_dlssnr.dll`; they belong to the add-on shim those projects load
and map onto the `DLSSNR.*` names above, `NRStyle` onto `Style`, `NRGlobalTone`
onto `LocalToneStrength`, `NREnableUpscaling` onto `ScalingRatio`.

For completeness the same scan finds no occurrence anywhere in the 165 MB binary
of Generative, Radiance, Bounce, Indirect, Semantic, Hair, Fabric, Subsurface,
Material or Lighting, and `DLSSNR` is the only parameter namespace it contains.

Real parameters still unused, each a separate piece of work:

- `ScalingRatio` is the actual upscaling control, not the `Upscaling` flag this
  project used to set.
- `ControlMask` takes a mask texture, so material specific behaviour such as
  `SkinStructureStrength` can be driven explicitly instead of relying on
  `UseAutoMask` to classify the frame.
- `Backbuffer`, `UI` and `UIAlpha` drive the UI compositing path.
- `BidirectionalDistortionField` handles lens distortion.

## Generative neural rendering

DLSS 5's headline capability, a generative stage that repaints lighting,
materials and shadowing on the finished frame without touching geometry, is what
this feature is. "NR" is Neural Rendering, not noise reduction, and the parameter
surface matches the capability rather than a denoiser: `Intensity` for effect
strength, `Style` and `LocalToneStrength` for grading, `UseAutoMask` and
`ControlMask` for restricting where it applies, and `SkinStructureStrength` for
material specific behaviour.

It is on by default here, at full strength. `Intensity` is the master control:

```text
intensity 0.00   26.15% different from the default
intensity 0.25   15.43%
intensity 0.50    9.14%
intensity 0.75    4.15%
intensity 1.00     base
intensity 1.50    no change, the runtime clamps at 1.0
```

That was nearly missed. Tested only at 2.0 the parameter looks completely inert,
because it is above the clamp, and it was written off as ignored on that basis.
Sweeping the range shows it moving the image monotonically, and at 0.0 the
generative stage is off. `Hint.Render.Preset` really is inert, verified across
its whole 0..3 range rather than at one value.

What is not wired up is `ControlMask`, which would let a render restrict the
effect to chosen regions rather than relying on `UseAutoMask` to classify the
frame, and `ScalingRatio`.

## References

Parameter names, the capability-block calling convention, and the caller-gate approach were recovered by [DaniilSokolyuk/video2dlssnr](https://github.com/DaniilSokolyuk/video2dlssnr). Runtime version pinning and GPU gating follow [Merserk/dlss5-visual-enhancer](https://github.com/Merserk/dlss5-visual-enhancer). Neither project is affiliated with this one.

## License

Patch integration code uses Blender-compatible SPDX headers. Adapted bridge and caller shim retain MIT attribution from ComfyUI-DLSS5-NR. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
