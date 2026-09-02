# SPDX-License-Identifier: MIT

import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty, PointerProperty


class DLSS5NRSettings(bpy.types.PropertyGroup):
    style: EnumProperty(
        name="Style",
        items=[
            ("1", "Natural", "Natural reconstruction"),
            ("2", "Cinematic", "Cinematic reconstruction"),
            ("0", "Default", "Runtime default"),
            ("3", "Style 3", "Experimental style 3"),
            ("4", "Style 4", "Experimental style 4"),
            ("5", "Style 5", "Experimental style 5"),
            ("6", "Style 6", "Experimental style 6"),
        ],
        default="1",
    )
    preset: IntProperty(name="Preset", default=3, min=0, max=3)
    intensity: FloatProperty(name="Intensity", default=1.0, min=0.0, max=2.0, step=5)
    tone: FloatProperty(name="Tone", default=1.0, min=0.0, max=2.0, step=5)
    structure: FloatProperty(name="Structure", default=1.0, min=0.0, max=2.0, step=5)
    skin: FloatProperty(name="Skin", default=-1.0, min=-1.0, max=2.0, step=5)
    auto_mask: BoolProperty(name="Auto Mask", default=False)
    gpu_index: IntProperty(name="NVIDIA GPU", default=0, min=0, max=15)
    channel_order: EnumProperty(
        name="Channels",
        items=[
            ("auto", "Auto", "Choose channel order by comparing source colors"),
            ("RGBA", "RGBA", "Use returned channels directly"),
            ("BGRA", "BGRA", "Swap red and blue"),
        ],
        default="auto",
    )


CLASSES = (DLSS5NRSettings,)


def register_properties():
    bpy.types.Scene.dlss5nr = PointerProperty(type=DLSS5NRSettings)


def unregister_properties():
    del bpy.types.Scene.dlss5nr
