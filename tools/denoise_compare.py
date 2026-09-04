"""Render one fixed scene with several denoisers and report HDR statistics.

Cycles renders are scene-referred and unbounded, but DLSS 5 NR is a
display-referred model, so everything interesting about this integration
happens above 1.0. The scene is therefore built with emissives far brighter
than white, and the numbers below are chosen to show what a denoiser does to
that range rather than how it looks in a screenshot.

Run inside Blender:

    blender -b -P tools/denoise_compare.py -- --out DIR [--device GPU]
                                              [--samples 32]
                                              [--denoisers NONE,OPENIMAGEDENOISE,DLSS5NR]

DLSS5NR additionally needs CYCLES_DLSS5NR_BRIDGE and CYCLES_DLSS5NR_RUNTIME set,
and an OptiX device; see RUNTIME_SETUP.md.
"""

from __future__ import annotations

import argparse
import sys

import bpy
import numpy as np


def parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(prog="denoise_compare")
    parser.add_argument("--out", required=True, help="directory for the EXR files")
    parser.add_argument("--device", default="GPU", choices=("GPU", "CPU"))
    parser.add_argument("--samples", type=int, default=32)
    parser.add_argument(
        "--denoisers",
        default="NONE,OPENIMAGEDENOISE,DLSS5NR",
        help="comma separated; NONE renders without denoising",
    )
    return parser.parse_args(argv)


def build_scene(device: str, samples: int, denoiser: str) -> None:
    bpy.ops.wm.read_factory_settings(use_empty=True)

    # Without this the addon preferences keep compute_device_type='NONE', Cycles
    # finds no GPU, and it quietly falls back - including falling back off
    # DLSS 5 NR, which only binds to an OptiX or CUDA device. A silent fallback
    # here reads as "the denoiser ran and changed nothing", which is worse than
    # an error, so refuse to continue instead.
    prefs = bpy.context.preferences.addons["cycles"].preferences
    if device == "GPU":
        prefs.compute_device_type = "OPTIX"
        prefs.get_devices()
        for candidate in prefs.devices:
            candidate.use = candidate.type == "OPTIX"
        if not any(candidate.use for candidate in prefs.devices):
            raise SystemExit("no OptiX device available; a GPU comparison would silently use the CPU")

    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = device
    scene.cycles.samples = samples
    scene.cycles.seed = 0
    scene.cycles.use_denoising = denoiser != "NONE"
    if denoiser != "NONE":
        scene.cycles.denoiser = denoiser
    scene.render.resolution_x = 256
    scene.render.resolution_y = 256
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_depth = "32"
    # Standard, not Filmic: a view transform with a highlight rolloff would hide
    # exactly the clipping this script exists to measure.
    scene.view_settings.view_transform = "Standard"

    bpy.ops.mesh.primitive_plane_add(size=20, location=(0, 0, 0))
    bpy.ops.mesh.primitive_uv_sphere_add(radius=1.2, location=(0, 0, 1.2))
    bpy.ops.object.shade_smooth()

    material = bpy.data.materials.new("Emit")
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()
    emission = tree.nodes.new("ShaderNodeEmission")
    emission.inputs["Strength"].default_value = 25.0
    emission.inputs["Color"].default_value = (1.0, 0.6, 0.2, 1.0)
    output = tree.nodes.new("ShaderNodeOutputMaterial")
    tree.links.new(emission.outputs["Emission"], output.inputs["Surface"])
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.45, location=(1.6, -1.2, 0.6))
    bpy.context.object.data.materials.append(material)

    light_data = bpy.data.lights.new("L", "AREA")
    light_data.energy = 400
    light_data.size = 3
    light = bpy.data.objects.new("L", light_data)
    light.location = (4, -4, 6)
    scene.collection.objects.link(light)

    camera_data = bpy.data.cameras.new("C")
    camera = bpy.data.objects.new("C", camera_data)
    camera.location = (6, -6, 4)
    camera.rotation_euler = (1.1, 0, 0.78)
    scene.collection.objects.link(camera)
    scene.camera = camera


def render(path: str) -> np.ndarray:
    bpy.context.scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(path + ".exr")
    return np.array(image.pixels[:], dtype=np.float32).reshape(-1, 4)[:, :3]


def main() -> int:
    args = parse_args()
    results: dict[str, np.ndarray] = {}

    for denoiser in [name.strip().upper() for name in args.denoisers.split(",") if name.strip()]:
        build_scene(args.device, args.samples, denoiser)
        resolved = bpy.context.scene.cycles.denoiser if denoiser != "NONE" else "NONE"
        if denoiser != "NONE" and resolved != denoiser:
            # Cycles accepted the assignment but fell back to something else.
            print(f"  {denoiser}: SKIPPED, Cycles resolved it to {resolved}")
            continue
        try:
            results[denoiser] = render(f"{args.out}/{denoiser.lower()}_{args.device.lower()}")
        except Exception as error:  # a denoiser that cannot run should not hide the others
            print(f"  {denoiser}: FAILED, {error}")

    if not results:
        print("no renders completed")
        return 1

    print("")
    print(f"{'denoiser':<18}{'max':>10}{'mean':>10}{'p99':>10}{'frac>1':>10}{'sat@1.0':>10}")
    for name, pixels in results.items():
        print(
            f"{name:<18}{pixels.max():>10.4f}{pixels.mean():>10.4f}"
            f"{np.percentile(pixels, 99):>10.4f}{(pixels > 1.0).mean():>10.4f}"
            f"{np.isclose(pixels, 1.0, atol=1e-6).mean():>10.4f}"
        )

    # OpenImageDenoise is the reference: it is a mature HDR denoiser that leaves
    # the range alone, so differences against it separate "this model is worse"
    # from "this staging path threw the range away".
    reference = results.get("OPENIMAGEDENOISE")
    noisy = results.get("NONE")
    if reference is not None:
        print("")
        for name, pixels in results.items():
            if name == "OPENIMAGEDENOISE":
                continue
            print(f"{name} vs OPENIMAGEDENOISE:")
            print(f"  energy kept = {pixels.mean() / reference.mean():.3f}")
            if noisy is not None:
                in_range = (noisy <= 1.0) & (reference <= 1.0)
                if in_range.any():
                    print(
                        f"  below 1.0: corr = {np.corrcoef(pixels[in_range], reference[in_range])[0, 1]:.4f}"
                        f", mean abs diff = {np.abs(pixels[in_range] - reference[in_range]).mean():.4f}"
                    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
