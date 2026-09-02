# SPDX-License-Identifier: MIT

import bpy

from .native_bridge import runtime_status
from .operators import get_render_result


class DLSS5NR_PT_image_editor(bpy.types.Panel):
    bl_label = "DLSS 5 NR"
    bl_idname = "DLSS5NR_PT_image_editor"
    bl_space_type = "IMAGE_EDITOR"
    bl_region_type = "UI"
    bl_category = "DLSS 5 NR"

    def draw(self, context):
        layout = self.layout
        settings = context.scene.dlss5nr
        ready, status = runtime_status()

        status_box = layout.box()
        status_box.label(text=status, icon="CHECKMARK" if ready else "ERROR")
        if get_render_result() is None:
            status_box.label(text="Render Result is empty", icon="INFO")

        column = layout.column(align=True)
        column.prop(settings, "style")
        column.prop(settings, "preset")
        column.prop(settings, "intensity")
        column.prop(settings, "tone")
        column.prop(settings, "structure")
        column.prop(settings, "skin")
        column.prop(settings, "auto_mask")

        advanced = layout.column(align=True)
        advanced.prop(settings, "gpu_index")
        advanced.prop(settings, "channel_order")

        action = layout.column()
        action.enabled = ready and get_render_result() is not None
        action.operator("image.dlss5nr_denoise_render_result", icon="RENDER_STILL")

        warning = layout.box()
        warning.alert = True
        warning.label(text="Experimental undocumented API")
        warning.label(text="Save work before processing")


CLASSES = (DLSS5NR_PT_image_editor,)
