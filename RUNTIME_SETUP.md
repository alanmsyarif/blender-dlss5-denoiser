# Runtime setup

Obtain compatible NVIDIA runtime files through a license that permits your use. This project does not provide, download, or redistribute them.

The tested `nvngx_dlssnr.dll` is version `310.8.0.0`, FileVersion `310.8.SF.0`, product name `NVIDIA DLSSNR`. Check those fields before use. Other versions may move the parameter-block ABI the bridge probes at startup.

Create this layout outside the repository or inside an unpacked local build:

```text
dlss5nr_runtime/
  nvngx_dlssnr.dll
  caller/
    nvngx.dll_blender.dll
```

`nvngx.dll_blender.dll` is the caller shim, built by `native\build_native.ps1`
into `runtime\caller`. A shim named `nvngx.dll_comfy.dll` or `nvngx.dll` is also
accepted, in that order, for runtimes prepared for other projects.

`_nvngx.dll` is the driver's NGX core and does **not** need to be placed here.
The bridge looks for `_nvngx.dll` beside the runtime first, then on the normal
search path, then in the NVIDIA DriverStore packages matching `nv*.inf_*`, which
is where a normal driver install leaves it. Copy it in only if that search fails.

Set environment variables before launching Blender:

```powershell
$env:CYCLES_DLSS5NR_BRIDGE = "C:\path\to\blender\dlss5nr_bridge.dll"
$env:CYCLES_DLSS5NR_RUNTIME = "C:\path\to\blender\dlss5nr_runtime"
.\blender.exe
```

The bridge loads `nvngx_dlssnr.dll` with `LOAD_WITH_ALTERED_SEARCH_PATH`, so that DLL's own dependencies resolve from the directory it sits in. Keep the runtime directory writable only by you: anything droppable next to `nvngx_dlssnr.dll` can be loaded into Blender's process. The same applies to any runtime you did not obtain yourself - verify it before use, since a DLL loaded this way runs with your full privileges and Authenticode `HashMismatch` means the binary was modified after NVIDIA signed it.

Then in Cycles choose **DLSS 5 NR (Experimental)** as the denoiser, for final
renders or the viewport. The extra controls that appear are Style, Intensity,
Tone, Structure, Auto Mask and Skin; Skin only does anything with Auto Mask on,
and Intensity is clamped at 1.0 by the runtime, where 0.0 disables the neural
effect and leaves plain reconstruction.

Nothing else needs enabling. The denoiser asks Cycles for the depth pass itself
and derives motion vectors by reprojecting between evaluations, so the Vector
pass is not required. The console reports which mode is active:

```text
DLSS 5 NR guides active: depth and motion, temporal accumulation on
```

Do not rename NVIDIA DLLs. Keep bridge, shim, and NVIDIA runtime versions together. If initialization fails, check Blender's console for the exact bridge error and switch Cycles back to OptiX or OpenImageDenoise.
