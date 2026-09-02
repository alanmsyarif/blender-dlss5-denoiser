# Runtime setup

Obtain compatible NVIDIA runtime files through a license that permits your use. This project does not provide, download, or redistribute them.

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

Do not rename NVIDIA DLLs. Keep bridge, shim, and NVIDIA runtime versions together. If initialization fails, check Blender's console for the exact bridge error and switch Cycles back to OptiX or OpenImageDenoise.
