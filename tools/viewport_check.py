"""Drive a real rendered viewport with a chosen denoiser, then quit.

Viewport denoising is not reachable from a background render. It differs from a
final render in how it calls the denoiser rather than in which code it calls:
many invocations, view changes that set the reset flag, and region resizes that
change the denoise resolution mid-session. Every bug this project has hit in the
denoiser lifetime showed up only under that pattern.

Needs a display, so run it without -b:

    blender --factory-startup --debug-cycles -P tools/viewport_check.py -- [--denoiser DLSS5NR]
                                                                          [--orbits 10] [--interval 0.7]

DLSS5NR additionally needs CYCLES_DLSS5NR_BRIDGE and CYCLES_DLSS5NR_RUNTIME set
and an OptiX device; see RUNTIME_SETUP.md. Blender exits by itself when done.

Read the result from the console. A clean run ends with VIEWPORT_CHECK_DONE and
no failures; grep the output for EXCEPTION_ACCESS_VIOLATION, DEVICE_HUNG and
"Could not create a D3D12 device", which are the three ways this has broken
before and none of which stop the session on their own.

What it cannot tell you is whether the result looks stable. With cleared depth
and motion guides the model is a single frame spatial denoiser, so temporal
flicker during the orbit is expected and has to be judged by eye.
"""

from __future__ import annotations

import argparse
import sys

import bpy
import mathutils


def parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(prog="viewport_check")
    parser.add_argument("--denoiser", default="DLSS5NR")
    parser.add_argument("--orbits", type=int, default=10)
    parser.add_argument("--interval", type=float, default=0.7)
    return parser.parse_args(argv)


def viewport_area():
    for area in bpy.context.screen.areas:
        if area.type == "VIEW_3D":
            return area
    return None


def setup(denoiser: str) -> None:
    prefs = bpy.context.preferences.addons["cycles"].preferences
    prefs.compute_device_type = "OPTIX"
    prefs.get_devices()
    for device in prefs.devices:
        device.use = device.type == "OPTIX"
    enabled = [device.name for device in prefs.devices if device.use]
    print("VIEWPORT optix:", enabled)
    if not enabled:
        raise SystemExit("no OptiX device; the viewport would fall back and prove nothing")

    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "GPU"
    scene.cycles.preview_samples = 8
    scene.cycles.use_preview_denoising = denoiser != "NONE"
    if denoiser != "NONE":
        scene.cycles.preview_denoiser = denoiser
        if scene.cycles.preview_denoiser != denoiser:
            raise SystemExit(f"Cycles resolved preview_denoiser to {scene.cycles.preview_denoiser}")
    print("VIEWPORT preview_denoiser:", scene.cycles.preview_denoiser)

    # Bright enough to exercise the HDR staging path, as in denoise_compare.py.
    material = bpy.data.materials.new("Emit")
    tree = material.node_tree
    tree.nodes.clear()
    emission = tree.nodes.new("ShaderNodeEmission")
    emission.inputs["Strength"].default_value = 25.0
    output = tree.nodes.new("ShaderNodeOutputMaterial")
    tree.links.new(emission.outputs["Emission"], output.inputs["Surface"])
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.6, location=(1.6, -1.2, 0.6))
    bpy.context.object.data.materials.append(material)


def build_steps(orbits: int) -> list:
    def start():
        viewport_area().spaces[0].shading.type = "RENDERED"
        print("VIEWPORT shading: RENDERED")

    def orbit(delta):
        def run():
            region = viewport_area().spaces[0].region_3d
            region.view_rotation = (
                mathutils.Quaternion((0.0, 0.0, 1.0), delta) @ region.view_rotation
            )
            region.view_distance *= 1.0 + delta * 0.2
            viewport_area().tag_redraw()
        return run

    def resize(show_ui, show_toolbar):
        # Toggling these regions resizes the 3D region, which is what dragging an
        # area boundary does to the denoiser: a new resolution mid-session.
        def run():
            space = viewport_area().spaces[0]
            space.show_region_ui = show_ui
            space.show_region_toolbar = show_toolbar
            viewport_area().tag_redraw()
            print(f"VIEWPORT resize: ui={show_ui} toolbar={show_toolbar}")
        return run

    steps = [start]
    per_leg = max(1, orbits // 3)
    steps += [orbit(0.15)] * per_leg
    steps.append(resize(True, True))
    steps += [orbit(-0.2)] * per_leg
    steps.append(resize(False, False))
    steps += [orbit(0.1)] * (orbits - 2 * per_leg)
    steps.append(resize(True, False))
    return steps


def main() -> None:
    args = parse_args()
    setup(args.denoiser)
    steps = build_steps(args.orbits)
    state = {"index": 0, "failures": 0}

    def tick():
        index = state["index"]
        if index >= len(steps):
            print(f"VIEWPORT_CHECK_DONE steps={len(steps)} failures={state['failures']}")
            bpy.ops.wm.quit_blender()
            return None
        try:
            steps[index]()
        except Exception as error:
            state["failures"] += 1
            print(f"VIEWPORT step {index} FAILED: {error}")
        state["index"] = index + 1
        return args.interval

    # A first interval gives the window time to open and the engine to start.
    bpy.app.timers.register(tick, first_interval=2.0)
    print(f"VIEWPORT driving {len(steps)} steps at {args.interval}s")


main()
