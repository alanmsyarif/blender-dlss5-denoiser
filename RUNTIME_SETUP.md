# Runtime setup

Obtain compatible NVIDIA runtime files through a license that permits your use. This project does not provide, download, or redistribute them.

The tested `nvngx_dlssnr.dll` is version `310.8.0.0`, FileVersion `310.8.SF.0`, product name `NVIDIA DLSSNR`. Check those fields before use. Other versions may move the parameter-block ABI the bridge probes at startup.

Create this layout outside the repository or inside an unpacked local build:

```text
dlss5nr_runtime/
  nvngx_dlssnr.dll
  _nvngx.dll
  caller/
    nvngx.dll_comfy.dll
```

Set environment variables before launching Blender:

```powershell
$env:CYCLES_DLSS5NR_BRIDGE = "C:\path\to\blender\dlss5nr_bridge.dll"
$env:CYCLES_DLSS5NR_RUNTIME = "C:\path\to\blender\dlss5nr_runtime"
.\blender.exe
```

The bridge loads `nvngx_dlssnr.dll` with `LOAD_WITH_ALTERED_SEARCH_PATH`, so that DLL's own dependencies resolve from the directory it sits in. Keep the runtime directory writable only by you: anything droppable next to `nvngx_dlssnr.dll` can be loaded into Blender's process. The same applies to any runtime you did not obtain yourself - verify it before use, since a DLL loaded this way runs with your full privileges and Authenticode `HashMismatch` means the binary was modified after NVIDIA signed it.

Do not rename NVIDIA DLLs. Keep bridge, shim, and NVIDIA runtime versions together. If initialization fails, check Blender's console for the exact bridge error and switch Cycles back to OptiX or OpenImageDenoise.
