# SPDX-License-Identifier: MIT

import ctypes
import os
import platform
import threading
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent
BRIDGE = ROOT / "native" / "bin" / "dlss5nr_bridge.dll"
RUNTIME = ROOT / "runtime"
SHIM_NAMES = ("nvngx.dll_blender.dll", "nvngx.dll_comfy.dll", "nvngx.dll")

_lock = threading.RLock()
_lib = None
_initialized_gpu = None
_dll_directory_handles = []


class DLSS5NRError(RuntimeError):
    pass


def runtime_status():
    if platform.system() != "Windows":
        return False, "Windows x64 only"
    if not BRIDGE.exists():
        return False, "Native bridge missing; run build_native.bat"
    if not (RUNTIME / "nvngx_dlssnr.dll").exists():
        return False, "Place nvngx_dlssnr.dll in runtime"
    if not any((RUNTIME / "caller" / name).exists() for name in SHIM_NAMES):
        return False, "Caller shim missing; run build_native.bat"
    return True, "Runtime files found"


def _decode_error(buffer):
    return buffer.value.decode("utf-8", errors="replace") or "Unknown native error"


def _load_library():
    global _lib
    if _lib is not None:
        return _lib

    ready, message = runtime_status()
    if not ready:
        raise DLSS5NRError(message)

    for dll_dir in (BRIDGE.parent, RUNTIME, RUNTIME / "caller"):
        if dll_dir.exists():
            _dll_directory_handles.append(os.add_dll_directory(str(dll_dir)))

    try:
        lib = ctypes.WinDLL(str(BRIDGE))
    except OSError as error:
        raise DLSS5NRError(f"Could not load native bridge: {error}") from error

    lib.dlss5nr_init.argtypes = [ctypes.c_int, ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_int]
    lib.dlss5nr_init.restype = ctypes.c_int
    lib.dlss5nr_process.argtypes = [
        ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_float,
        ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_int,
    ]
    lib.dlss5nr_process.restype = ctypes.c_int
    lib.dlss5nr_shutdown.argtypes = []
    lib.dlss5nr_shutdown.restype = None
    lib.dlss5nr_version.argtypes = []
    lib.dlss5nr_version.restype = ctypes.c_char_p
    lib.dlss5nr_gpu_name.argtypes = []
    lib.dlss5nr_gpu_name.restype = ctypes.c_char_p
    _lib = lib
    return lib


def _ensure_initialized(gpu_index):
    global _initialized_gpu
    lib = _load_library()
    if _initialized_gpu == gpu_index:
        return lib
    if _initialized_gpu is not None:
        lib.dlss5nr_shutdown()
        _initialized_gpu = None

    error = ctypes.create_string_buffer(4096)
    if not lib.dlss5nr_init(int(gpu_index), str(RUNTIME), error, len(error)):
        raise DLSS5NRError(_decode_error(error))
    _initialized_gpu = gpu_index
    return lib


def process(rgb, *, style, preset, intensity, tone, structure, skin, auto_mask, gpu_index):
    source = np.ascontiguousarray(rgb, dtype=np.float32)
    if source.ndim != 3 or source.shape[2] != 3:
        raise DLSS5NRError("Expected RGB float array shaped [height, width, 3]")
    height, width, _ = source.shape
    output = np.empty_like(source)

    with _lock:
        lib = _ensure_initialized(int(gpu_index))
        error = ctypes.create_string_buffer(4096)
        ok = lib.dlss5nr_process(
            source.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
            output.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
            width, height, int(style), int(preset),
            ctypes.c_float(float(intensity)), ctypes.c_float(float(tone)),
            ctypes.c_float(float(structure)), ctypes.c_float(float(skin)),
            int(bool(auto_mask)), 1, error, len(error),
        )
        if not ok:
            raise DLSS5NRError(_decode_error(error))
    return output


def runtime_info(gpu_index):
    with _lock:
        lib = _ensure_initialized(int(gpu_index))
        version = lib.dlss5nr_version()
        gpu = lib.dlss5nr_gpu_name()
        return {
            "bridge": version.decode("utf-8", errors="replace") if version else "unknown",
            "gpu": gpu.decode("utf-8", errors="replace") if gpu else "unknown",
        }


def shutdown():
    global _initialized_gpu
    with _lock:
        if _lib is not None and _initialized_gpu is not None:
            _lib.dlss5nr_shutdown()
        _initialized_gpu = None
