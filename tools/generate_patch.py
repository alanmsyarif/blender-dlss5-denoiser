from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path

EXPECTED_COMMIT = "a3db93c5b2595a79f65f304114c23aeef8c2139f"


def replace(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if old not in text:
        raise RuntimeError(f"Expected source block not found in {path}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("blender", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = args.blender.resolve()
    project = Path(__file__).resolve().parents[1]

    commit = subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", "HEAD"], text=True
    ).strip()
    if commit != EXPECTED_COMMIT:
        raise RuntimeError(f"Expected Blender v5.0.1 {EXPECTED_COMMIT}, got {commit}")
    if subprocess.check_output(["git", "-C", str(root), "status", "--porcelain"], text=True):
        raise RuntimeError("Blender checkout must be clean")

    destination = root / "intern/cycles/integrator"
    shutil.copy2(project / "source/dlss5nr/denoiser_dlss5nr.h", destination)
    shutil.copy2(project / "source/dlss5nr/denoiser_dlss5nr.cpp", destination)

    replace(
        root / "CMakeLists.txt",
        'option(WITH_CYCLES_DEBUG "Build Cycles with options useful for debugging (e.g., MIS)" OFF)\n',
        'option(WITH_CYCLES_DEBUG "Build Cycles with options useful for debugging (e.g., MIS)" OFF)\n'
        'option(WITH_CYCLES_DLSS5_NR "Build experimental Windows-only DLSS 5 NR denoiser" OFF)\n'
        'if(WITH_CYCLES_DLSS5_NR AND NOT WIN32)\n'
        '  message(FATAL_ERROR "WITH_CYCLES_DLSS5_NR is supported only on Windows")\n'
        'endif()\n',
    )
    replace(
        root / "intern/cycles/CMakeLists.txt",
        "if(WITH_CYCLES_DEBUG)\n  add_definitions(-DWITH_CYCLES_DEBUG)\nendif()\n",
        "if(WITH_CYCLES_DEBUG)\n  add_definitions(-DWITH_CYCLES_DEBUG)\nendif()\n"
        "if(WITH_CYCLES_DLSS5_NR)\n  add_definitions(-DWITH_CYCLES_DLSS5_NR)\nendif()\n",
    )
    replace(
        root / "intern/cycles/integrator/CMakeLists.txt",
        "  denoiser_optix.cpp\n",
        "  denoiser_optix.cpp\n",
    )
    replace(
        root / "intern/cycles/integrator/CMakeLists.txt",
        "if(WITH_OPENIMAGEDENOISE)\n",
        "if(WITH_CYCLES_DLSS5_NR)\n"
        "  list(APPEND SRC denoiser_dlss5nr.cpp)\n"
        "  list(APPEND SRC_HEADERS denoiser_dlss5nr.h)\n"
        "endif()\n\n"
        "if(WITH_OPENIMAGEDENOISE)\n",
    )
    replace(
        root / "intern/cycles/device/denoise.h",
        "  DENOISER_OPENIMAGEDENOISE = 4,\n  DENOISER_NUM,\n",
        "  DENOISER_OPENIMAGEDENOISE = 4,\n  DENOISER_DLSS5NR = 8,\n  DENOISER_NUM,\n",
    )
    replace(
        root / "intern/cycles/device/denoise.cpp",
        '    case DENOISER_OPENIMAGEDENOISE:\n      return "OpenImageDenoise";\n',
        '    case DENOISER_OPENIMAGEDENOISE:\n      return "OpenImageDenoise";\n'
        '    case DENOISER_DLSS5NR:\n      return "DLSS 5 NR (Experimental)";\n',
    )
    replace(
        root / "intern/cycles/device/denoise.cpp",
        '    type_enum.insert("openimageio", DENOISER_OPENIMAGEDENOISE);\n',
        '    type_enum.insert("openimageio", DENOISER_OPENIMAGEDENOISE);\n'
        '    type_enum.insert("dlss5nr", DENOISER_DLSS5NR);\n',
    )
    replace(
        root / "intern/cycles/scene/integrator.cpp",
        '  denoiser_type_enum.insert("openimagedenoise", DENOISER_OPENIMAGEDENOISE);\n',
        '  denoiser_type_enum.insert("openimagedenoise", DENOISER_OPENIMAGEDENOISE);\n'
        '  denoiser_type_enum.insert("dlss5nr", DENOISER_DLSS5NR);\n',
    )
    replace(
        root / "intern/cycles/device/optix/device.cpp",
        "    info.denoisers |= DENOISER_OPTIX;\n",
        "    info.denoisers |= DENOISER_OPTIX;\n"
        "#  ifdef WITH_CYCLES_DLSS5_NR\n"
        "    info.denoisers |= DENOISER_DLSS5NR;\n"
        "#  endif\n",
    )
    replace(
        root / "intern/cycles/integrator/denoiser.cpp",
        '#include "integrator/denoiser_oidn.h"\n',
        '#include "integrator/denoiser_oidn.h"\n'
        '#ifdef WITH_CYCLES_DLSS5_NR\n'
        '#  include "integrator/denoiser_dlss5nr.h"\n'
        '#endif\n',
    )
    replace(
        root / "intern/cycles/integrator/denoiser.cpp",
        "bool use_gpu_oidn_denoiser(Device *denoiser_device, const DenoiseParams &params)\n",
        "bool use_dlss5nr_denoiser(Device *denoiser_device, const DenoiseParams &params)\n"
        "{\n"
        "#ifdef WITH_CYCLES_DLSS5_NR\n"
        "  return params.type == DENOISER_DLSS5NR &&\n"
        "         DLSS5NRDenoiser::is_device_supported(denoiser_device->info);\n"
        "#else\n"
        "  (void)denoiser_device;\n"
        "  (void)params;\n"
        "  return false;\n"
        "#endif\n"
        "}\n\n"
        "bool use_gpu_oidn_denoiser(Device *denoiser_device, const DenoiseParams &params)\n",
    )
    replace(
        root / "intern/cycles/integrator/denoiser.cpp",
        "    if (use_optix_denoiser(single_denoiser_device, effective_denoise_params) ||\n        use_gpu_oidn_denoiser(single_denoiser_device, effective_denoise_params))\n",
        "    if (use_optix_denoiser(single_denoiser_device, effective_denoise_params) ||\n"
        "        use_dlss5nr_denoiser(single_denoiser_device, effective_denoise_params) ||\n"
        "        use_gpu_oidn_denoiser(single_denoiser_device, effective_denoise_params))\n",
    )
    replace(
        root / "intern/cycles/integrator/denoiser.cpp",
        "  if (is_cpu_denoiser_device == false) {\n#ifdef WITH_OPTIX\n",
        "  if (is_cpu_denoiser_device == false) {\n"
        "#ifdef WITH_CYCLES_DLSS5_NR\n"
        "    if (use_dlss5nr_denoiser(single_denoiser_device, effective_denoiser_params)) {\n"
        "      return make_unique<DLSS5NRDenoiser>(single_denoiser_device, effective_denoiser_params);\n"
        "    }\n"
        "#endif\n\n"
        "#ifdef WITH_OPTIX\n",
    )
    replace(
        root / "intern/cycles/blender/python.cpp",
        "  if (ccl::openimagedenoise_supported()) {\n",
        "#ifdef WITH_CYCLES_DLSS5_NR\n"
        '  PyModule_AddObjectRef(mod, "with_dlss5nr", Py_True);\n'
        "#else\n"
        '  PyModule_AddObjectRef(mod, "with_dlss5nr", Py_False);\n'
        "#endif\n\n"
        "  if (ccl::openimagedenoise_supported()) {\n",
    )
    properties = root / "intern/cycles/blender/addon/properties.py"
    replace(
        properties,
        "def enum_optix_denoiser(self, context):\n",
        "def enum_dlss5nr_denoiser(self, context):\n"
        "    import _cycles\n"
        "    if _cycles.with_dlss5nr and (not context or bool(context.preferences.addons[__package__].preferences.get_devices_for_type('OPTIX'))):\n"
        "        return [('DLSS5NR', \"DLSS 5 NR (Experimental)\", n_(\"Use experimental same-resolution DLSS 5 noise reduction; requires external runtime\"), 8)]\n"
        "    return []\n\n\n"
        "def enum_optix_denoiser(self, context):\n",
    )
    replace(
        properties,
        "    items += optix_items\n    items += oidn_items\n",
        "    items += enum_dlss5nr_denoiser(self, context)\n"
        "    items += optix_items\n"
        "    items += oidn_items\n",
    )
    replace(
        properties,
        "    items += enum_optix_denoiser(self, context)\n    items += enum_openimagedenoise_denoiser(self, context)\n",
        "    items += enum_dlss5nr_denoiser(self, context)\n"
        "    items += enum_optix_denoiser(self, context)\n"
        "    items += enum_openimagedenoise_denoiser(self, context)\n",
    )

    subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "add",
            "-N",
            "intern/cycles/integrator/denoiser_dlss5nr.cpp",
            "intern/cycles/integrator/denoiser_dlss5nr.h",
        ],
        check=True,
    )
    diff = subprocess.check_output(
        ["git", "-C", str(root), "diff", "--binary", "--", "."], text=True
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(diff, encoding="utf-8")
    if not diff.strip():
        raise RuntimeError("Generated patch is empty")
    print(args.output)


if __name__ == "__main__":
    main()
