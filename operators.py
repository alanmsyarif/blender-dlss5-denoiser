# SPDX-License-Identifier: MIT

import bpy
import numpy as np

from .native_bridge import DLSS5NRError, process, runtime_info
from .pixels import choose_channel_order, join_rgba, split_rgba

OUTPUT_NAME = "DLSS 5 NR Result"


def get_render_result():
    image = bpy.data.images.get("Render Result")
    if image is None or not image.has_data or image.size[0] < 1 or image.size[1] < 1:
        return None
    return image


def show_image(context, image):
    if context.area and context.area.type == "IMAGE_EDITOR":
        context.area.spaces.active.image = image
        return
    if context.screen:
        for area in context.screen.areas:
            if area.type == "IMAGE_EDITOR":
                area.spaces.active.image = image
                return


class DLSS5NR_OT_denoise_render_result(bpy.types.Operator):
    bl_idname = "image.dlss5nr_denoise_render_result"
    bl_label = "Denoise Render Result"
    bl_description = "Process current Render Result into a new image using experimental DLSS 5 NR"
    bl_options = {"REGISTER"}

    @classmethod
    def poll(cls, _context):
        return get_render_result() is not None

    def execute(self, context):
        source_image = get_render_result()
        if source_image is None:
            self.report({"ERROR"}, "Render Result is empty. Render an image first.")
            return {"CANCELLED"}

        settings = context.scene.dlss5nr
        width, height = source_image.size[:]
        flat = np.empty(width * height * 4, dtype=np.float32)

        if context.window:
            context.window.cursor_set("WAIT")
        try:
            source_image.pixels.foreach_get(flat)
            source_rgb, alpha = split_rgba(flat, width, height)
            raw_output = process(
                source_rgb,
                style=int(settings.style),
                preset=settings.preset,
                intensity=settings.intensity,
                tone=settings.tone,
                structure=settings.structure,
                skin=settings.skin,
                auto_mask=settings.auto_mask,
                gpu_index=settings.gpu_index,
            )
            output_rgb = choose_channel_order(source_rgb, raw_output, settings.channel_order)
            output_pixels = join_rgba(output_rgb, alpha)

            old_image = bpy.data.images.get(OUTPUT_NAME)
            if old_image is not None and tuple(old_image.size) != (width, height):
                bpy.data.images.remove(old_image)
                old_image = None
            output_image = old_image or bpy.data.images.new(
                OUTPUT_NAME, width=width, height=height, alpha=True, float_buffer=True
            )
            output_image.pixels.foreach_set(output_pixels)
            output_image.update()
            show_image(context, output_image)

            info = runtime_info(settings.gpu_index)
            self.report({"INFO"}, f"Denoised with {info['gpu']}")
            return {"FINISHED"}
        except (DLSS5NRError, OSError, ValueError, RuntimeError) as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}
        finally:
            if context.window:
                context.window.cursor_set("DEFAULT")


CLASSES = (DLSS5NR_OT_denoise_render_result,)
