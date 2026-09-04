from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

EXPECTED_COMMIT = "a3db93c5b2595a79f65f304114c23aeef8c2139f"


def replace(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if old not in text:
        raise RuntimeError(f"Expected source block not found in {path}")
    # The explicit newline matters on Windows. read_text translates CRLF to LF,
    # and write_text would expand it back to the platform ending, rewriting
    # every line of a file we meant to touch in one place and burying the real
    # change inside a whole-file diff.
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")


def copy_lf(source: Path, destination: Path) -> None:
    """Copy a source file, normalising to LF.

    The working copies in source/dlss5nr are CRLF, Blender's tree is LF, and the
    patch has to be LF or git apply fails every hunk against a fresh checkout.
    """
    destination.write_text(source.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")


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
    copy_lf(project / "source/dlss5nr/denoiser_dlss5nr.h", destination / "denoiser_dlss5nr.h")
    copy_lf(project / "source/dlss5nr/denoiser_dlss5nr.cpp", destination / "denoiser_dlss5nr.cpp")

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

    # DLSS 5 NR appearance controls. The ABI exposes these and the model responds
    # to them, but nothing reached them from Cycles: the denoiser passed fixed
    # defaults for every one. Written as literal blocks rather than escaped
    # strings so the inserted C++ reads the way it will land in the file.
    replace(
        root / "intern/cycles/device/denoise.h",
        """  DenoiserPrefilter prefilter = DENOISER_PREFILTER_FAST;
  DenoiserQuality quality = DENOISER_QUALITY_HIGH;
""",
        """  DenoiserPrefilter prefilter = DENOISER_PREFILTER_FAST;
  DenoiserQuality quality = DENOISER_QUALITY_HIGH;

  /* DLSS 5 NR appearance controls. These reach the model directly and are
   * inert for every other denoiser. Style, structure and tone each move the
   * image far more than the denoiser's own run to run variation; skin does
   * nothing unless auto_mask is on, because the mask is what gives the model
   * something material specific to act on. */
  int dlss5nr_style = 1;
  float dlss5nr_tone = 1.0f;
  float dlss5nr_structure = 1.0f;
  /* Negative leaves the model's own default alone. */
  float dlss5nr_skin = -1.0f;
  bool dlss5nr_auto_mask = false;
""",
    )
    replace(
        root / "intern/cycles/device/denoise.cpp",
        """  SOCKET_ENUM(quality, "Quality", *quality_enum, DENOISER_QUALITY_HIGH);
""",
        """  SOCKET_ENUM(quality, "Quality", *quality_enum, DENOISER_QUALITY_HIGH);

  SOCKET_INT(dlss5nr_style, "DLSS5 NR Style", 1);
  SOCKET_FLOAT(dlss5nr_tone, "DLSS5 NR Tone", 1.0f);
  SOCKET_FLOAT(dlss5nr_structure, "DLSS5 NR Structure", 1.0f);
  SOCKET_FLOAT(dlss5nr_skin, "DLSS5 NR Skin", -1.0f);
  SOCKET_BOOLEAN(dlss5nr_auto_mask, "DLSS5 NR Auto Mask", false);
""",
    )
    replace(
        root / "intern/cycles/scene/integrator.h",
        """  NODE_SOCKET_API(DenoiserQuality, denoiser_quality);
""",
        """  NODE_SOCKET_API(DenoiserQuality, denoiser_quality);

  NODE_SOCKET_API(int, dlss5nr_style);
  NODE_SOCKET_API(float, dlss5nr_tone);
  NODE_SOCKET_API(float, dlss5nr_structure);
  NODE_SOCKET_API(float, dlss5nr_skin);
  NODE_SOCKET_API(bool, dlss5nr_auto_mask);
""",
    )
    integrator_cpp = root / "intern/cycles/scene/integrator.cpp"
    replace(
        integrator_cpp,
        """  SOCKET_ENUM(denoiser_quality, "Denoiser Quality", denoiser_quality_enum, DENOISER_QUALITY_HIGH);
""",
        """  SOCKET_ENUM(denoiser_quality, "Denoiser Quality", denoiser_quality_enum, DENOISER_QUALITY_HIGH);

  SOCKET_INT(dlss5nr_style, "DLSS5 NR Style", 1);
  SOCKET_FLOAT(dlss5nr_tone, "DLSS5 NR Tone", 1.0f);
  SOCKET_FLOAT(dlss5nr_structure, "DLSS5 NR Structure", 1.0f);
  SOCKET_FLOAT(dlss5nr_skin, "DLSS5 NR Skin", -1.0f);
  SOCKET_BOOLEAN(dlss5nr_auto_mask, "DLSS5 NR Auto Mask", false);
""",
    )
    replace(
        integrator_cpp,
        """  denoise_params.prefilter = denoiser_prefilter;
  denoise_params.quality = denoiser_quality;
""",
        """  denoise_params.prefilter = denoiser_prefilter;
  denoise_params.quality = denoiser_quality;

  denoise_params.dlss5nr_style = dlss5nr_style;
  denoise_params.dlss5nr_tone = dlss5nr_tone;
  denoise_params.dlss5nr_structure = dlss5nr_structure;
  denoise_params.dlss5nr_skin = dlss5nr_skin;
  denoise_params.dlss5nr_auto_mask = dlss5nr_auto_mask;
""",
    )
    sync_cpp = root / "intern/cycles/blender/sync.cpp"
    replace(
        sync_cpp,
        """  int input_passes = -1;
""",
        """  int input_passes = -1;

  /* One set of DLSS 5 NR controls for both final and viewport denoising: they
   * describe how the model should treat the image, not how much time to spend,
   * so splitting them per context would only invite the two to disagree. */
  denoising.dlss5nr_style = get_int(cscene, "dlss5nr_style");
  denoising.dlss5nr_tone = get_float(cscene, "dlss5nr_tone");
  denoising.dlss5nr_structure = get_float(cscene, "dlss5nr_structure");
  denoising.dlss5nr_skin = get_float(cscene, "dlss5nr_skin");
  denoising.dlss5nr_auto_mask = get_boolean(cscene, "dlss5nr_auto_mask");
""",
    )
    replace(
        sync_cpp,
        """    integrator->set_denoiser_quality(denoise_params.quality);
""",
        """    integrator->set_denoiser_quality(denoise_params.quality);

    integrator->set_dlss5nr_style(denoise_params.dlss5nr_style);
    integrator->set_dlss5nr_tone(denoise_params.dlss5nr_tone);
    integrator->set_dlss5nr_structure(denoise_params.dlss5nr_structure);
    integrator->set_dlss5nr_skin(denoise_params.dlss5nr_skin);
    integrator->set_dlss5nr_auto_mask(denoise_params.dlss5nr_auto_mask);
""",
    )

    replace(
        properties,
        "    denoiser: EnumProperty(",
        """    dlss5nr_style: EnumProperty(
        name="Style",
        description="Reconstruction style the DLSS 5 NR model applies",
        items=(
            ('DEFAULT', "Default", "Leave the runtime's own choice alone", 0),
            ('NATURAL', "Natural", "Natural reconstruction", 1),
            ('CINEMATIC', "Cinematic", "Cinematic reconstruction", 2),
        ),
        default='NATURAL',
    )
    dlss5nr_tone: FloatProperty(
        name="Tone",
        description="Local tone strength applied by the DLSS 5 NR model",
        min=0.0, max=2.0, default=1.0,
    )
    dlss5nr_structure: FloatProperty(
        name="Structure",
        description="Local structure strength applied by the DLSS 5 NR model",
        min=0.0, max=2.0, default=1.0,
    )
    dlss5nr_skin: FloatProperty(
        name="Skin",
        description=(
            "Structure strength for regions the model identifies as skin. "
            "Requires Auto Mask; below zero leaves the model default alone"
        ),
        min=-1.0, max=2.0, default=-1.0,
    )
    dlss5nr_auto_mask: BoolProperty(
        name="Auto Mask",
        description=(
            "Let the model classify the image so material specific controls "
            "such as Skin have something to act on"
        ),
        default=False,
    )

    denoiser: EnumProperty(""",
    )
    ui_py = root / "intern/cycles/blender/addon/ui.py"
    replace(
        ui_py,
        "def get_effective_preview_denoiser(context, has_oidn_gpu):",
        '''def draw_dlss5nr_options(layout, cscene):
    # Skin is only meaningful once the model is classifying the image, so it is
    # greyed out rather than hidden: hiding it makes the dependency invisible.
    layout.prop(cscene, "dlss5nr_style", text="Style")
    layout.prop(cscene, "dlss5nr_tone", text="Tone")
    layout.prop(cscene, "dlss5nr_structure", text="Structure")
    layout.prop(cscene, "dlss5nr_auto_mask", text="Auto Mask")
    sub = layout.column()
    sub.active = cscene.dlss5nr_auto_mask
    sub.prop(cscene, "dlss5nr_skin", text="Skin")


def get_effective_preview_denoiser(context, has_oidn_gpu):''',
    )
    replace(
        ui_py,
        '        col.prop(cscene, "preview_denoising_start_sample", text="Start Sample")\n',
        """        if effective_preview_denoiser == 'DLSS5NR':
            draw_dlss5nr_options(col, cscene)

        col.prop(cscene, "preview_denoising_start_sample", text="Start Sample")
""",
    )
    replace(
        ui_py,
        '        col.prop(cscene, "denoising_input_passes", text="Passes")\n',
        """        col.prop(cscene, "denoising_input_passes", text="Passes")
        if cscene.denoiser == 'DLSS5NR':
            draw_dlss5nr_options(col, cscene)
""",
    )

    # Intensity is the master strength of the neural rendering effect. It was
    # nearly left out: tested only at 2.0 it looked inert, because the runtime
    # clamps it at 1.0. Swept across 0 to 1 it moves the image monotonically,
    # and 0.0 turns the effect off entirely.
    replace(
        root / "intern/cycles/device/denoise.h",
        "  int dlss5nr_style = 1;\n",
        "  int dlss5nr_style = 1;\n  float dlss5nr_intensity = 1.0f;\n",
    )
    replace(
        root / "intern/cycles/device/denoise.cpp",
        '  SOCKET_INT(dlss5nr_style, "DLSS5 NR Style", 1);\n',
        '  SOCKET_INT(dlss5nr_style, "DLSS5 NR Style", 1);\n'
        '  SOCKET_FLOAT(dlss5nr_intensity, "DLSS5 NR Intensity", 1.0f);\n',
    )
    replace(
        root / "intern/cycles/scene/integrator.h",
        "  NODE_SOCKET_API(int, dlss5nr_style);\n",
        "  NODE_SOCKET_API(int, dlss5nr_style);\n  NODE_SOCKET_API(float, dlss5nr_intensity);\n",
    )
    replace(
        integrator_cpp,
        '  SOCKET_INT(dlss5nr_style, "DLSS5 NR Style", 1);\n',
        '  SOCKET_INT(dlss5nr_style, "DLSS5 NR Style", 1);\n'
        '  SOCKET_FLOAT(dlss5nr_intensity, "DLSS5 NR Intensity", 1.0f);\n',
    )
    replace(
        integrator_cpp,
        "  denoise_params.dlss5nr_style = dlss5nr_style;\n",
        "  denoise_params.dlss5nr_style = dlss5nr_style;\n"
        "  denoise_params.dlss5nr_intensity = dlss5nr_intensity;\n",
    )
    replace(
        sync_cpp,
        '  denoising.dlss5nr_style = get_int(cscene, "dlss5nr_style");\n',
        '  denoising.dlss5nr_style = get_int(cscene, "dlss5nr_style");\n'
        '  denoising.dlss5nr_intensity = get_float(cscene, "dlss5nr_intensity");\n',
    )
    replace(
        sync_cpp,
        "    integrator->set_dlss5nr_style(denoise_params.dlss5nr_style);\n",
        "    integrator->set_dlss5nr_style(denoise_params.dlss5nr_style);\n"
        "    integrator->set_dlss5nr_intensity(denoise_params.dlss5nr_intensity);\n",
    )
    replace(
        properties,
        "    dlss5nr_tone: FloatProperty(",
        '''    dlss5nr_intensity: FloatProperty(
        name="Intensity",
        description=(
            "Strength of the DLSS 5 neural rendering effect. The runtime clamps "
            "this at 1.0, so values above it change nothing; 0.0 turns the "
            "effect off and leaves the reconstruction alone"
        ),
        min=0.0, max=1.0, default=1.0,
    )
    dlss5nr_tone: FloatProperty(''',
    )
    replace(
        ui_py,
        '    layout.prop(cscene, "dlss5nr_style", text="Style")\n',
        '    layout.prop(cscene, "dlss5nr_style", text="Style")\n'
        '    layout.prop(cscene, "dlss5nr_intensity", text="Intensity")\n',
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
    # Same reason as replace(): the patch must stay LF or git apply fails
    # every hunk, which .gitattributes also pins with "*.patch -text".
    args.output.write_text(diff, encoding="utf-8", newline="\n")
    if not diff.strip():
        raise RuntimeError("Generated patch is empty")
    print(args.output)


if __name__ == "__main__":
    main()
