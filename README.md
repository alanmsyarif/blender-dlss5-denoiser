# Blender DLSS 5 Neural Rendering

Unofficial, experimental Blender 5.x extension that denoises current **Render Result** in-process with NVIDIA DLSS 5 Neural Rendering (NGX feature 18). Output is written to a separate float image named `DLSS 5 NR Result`; original render stays untouched.

Based on MIT-licensed native bridge work from [ComfyUI-DLSS5-NR](https://github.com/lisitskyaa/ComfyUI-DLSS5-NR). This project is not affiliated with or endorsed by NVIDIA or Blender Foundation.

## Requirements

- Windows 10/11 x64
- Blender 5.x
- NVIDIA RTX GPU and recent NVIDIA driver
- Visual Studio 2022 Build Tools with **Desktop development with C++**
- Legally obtained compatible `nvngx_dlssnr.dll`

This is same-resolution post-processing, not DLSS Super Resolution.

## Build

1. Place `nvngx_dlssnr.dll` in `runtime/`. Never commit or redistribute it.
2. Run `build_native.bat` on Windows. This creates:
   - `native/bin/dlss5nr_bridge.dll`
   - `runtime/caller/nvngx.dll_blender.dll`
3. Run `package_extension.bat`. Output appears in `dist/`.

`_nvngx.dll` is normally found in NVIDIA DriverStore. For troubleshooting only, a compatible local copy may be placed in `runtime/`.

## Install and use

1. In Blender: **Edit > Preferences > Get Extensions > Install from Disk**.
2. Select generated ZIP and enable extension.
3. Render still image.
4. Open Image Editor, then sidebar (`N`) > **DLSS 5 NR**.
5. Choose settings and click **Denoise Render Result**.

Every click resets temporal state because operator processes one still image. `Auto` channels handles known RGBA/BGRA differences between runtime builds.

## Troubleshooting

- **Native bridge missing**: run `build_native.bat`, package again, reinstall.
- **Runtime missing**: place `nvngx_dlssnr.dll` in extension `runtime/` before packaging.
- **NGX core unavailable**: update NVIDIA driver or test compatible `_nvngx.dll` local override.
- **Wrong red/blue channels**: choose `RGBA` or `BGRA` manually.
- **Feature creation/evaluation failed**: runtime, driver, or GPU may be incompatible.

## Warning

Feature uses undocumented NVIDIA interfaces. It may fail, produce incorrect output, or crash Blender. Save work before processing. Proprietary NVIDIA binaries and SDK headers are intentionally excluded; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
