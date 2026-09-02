# SPDX-License-Identifier: MIT

import numpy as np


def split_rgba(flat_pixels, width, height):
    rgba = np.asarray(flat_pixels, dtype=np.float32).reshape(height, width, 4)
    return np.ascontiguousarray(rgba[..., :3]), np.ascontiguousarray(rgba[..., 3])


def choose_channel_order(source, output, channel_order):
    if channel_order == "RGBA":
        return output
    swapped = output[..., [2, 1, 0]]
    if channel_order == "BGRA":
        return swapped

    height, width, _ = source.shape
    step_y = max(1, height // 128)
    step_x = max(1, width // 128)
    reference = source[::step_y, ::step_x]
    raw = output[::step_y, ::step_x]
    swap = swapped[::step_y, ::step_x]

    def score(candidate):
        pixel_error = np.mean(np.abs(candidate - reference))
        color_error = np.mean(np.abs(candidate.mean(axis=(0, 1)) - reference.mean(axis=(0, 1))))
        return float(pixel_error + color_error)

    return output if score(raw) <= score(swap) else swapped


def join_rgba(rgb, alpha):
    height, width, _ = rgb.shape
    rgba = np.empty((height, width, 4), dtype=np.float32)
    rgba[..., :3] = np.clip(rgb, 0.0, 1.0)
    rgba[..., 3] = alpha
    return rgba.ravel()
