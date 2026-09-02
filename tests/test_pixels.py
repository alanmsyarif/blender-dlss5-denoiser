import unittest

import numpy as np

from pixels import choose_channel_order, join_rgba, split_rgba


class PixelHelpersTest(unittest.TestCase):
    def test_round_trip_preserves_rgba(self):
        source = np.array([0.1, 0.2, 0.3, 0.4, 0.8, 0.7, 0.6, 0.5], dtype=np.float32)
        rgb, alpha = split_rgba(source, 2, 1)
        np.testing.assert_allclose(join_rgba(rgb, alpha), source)

    def test_explicit_bgra_swaps_red_and_blue(self):
        source = np.zeros((1, 1, 3), dtype=np.float32)
        output = np.array([[[0.9, 0.5, 0.1]]], dtype=np.float32)
        corrected = choose_channel_order(source, output, "BGRA")
        np.testing.assert_allclose(corrected, [[[0.1, 0.5, 0.9]]])

    def test_auto_prefers_source_color_statistics(self):
        source = np.array([[[0.9, 0.5, 0.1]]], dtype=np.float32)
        output = np.array([[[0.1, 0.5, 0.9]]], dtype=np.float32)
        corrected = choose_channel_order(source, output, "auto")
        np.testing.assert_allclose(corrected, source)


if __name__ == "__main__":
    unittest.main()
