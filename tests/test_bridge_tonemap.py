"""Guard the bridge's HDR staging path.

The bridge used to clamp colour to 0..1 before handing it to the model. On a
Cycles scene with ordinary emissives that flattened 16% of the image onto
exactly 1.0 and cost 49% of the total image energy, because Cycles renders are
scene-referred and unbounded. These tests pin the replacement in place.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BRIDGE = ROOT / "source/dlss5nr/dlss5nr_bridge.cpp"

# Mirrors kTonemapCeiling in the bridge: the largest value FP16 holds below 1.0.
CEILING = 1.0 - 1.0 / 2048.0


def forward(c):
    return c / (1.0 + c) if c > 0.0 else 0.0


def inverse(c):
    if c <= 0.0:
        return 0.0
    return min(c, CEILING) / (1.0 - min(c, CEILING))


class TonemapMathTests(unittest.TestCase):
    def test_forward_maps_all_positive_values_below_one(self):
        # Whatever Cycles produces has to land in the range sRGB and the model
        # accept, which is the property the old clamp provided by force.
        for value in (0.0, 0.5, 1.0, 25.0, 1e3, 1e6):
            self.assertLess(forward(value), 1.0)
            self.assertGreaterEqual(forward(value), 0.0)

    def test_round_trip_recovers_hdr_values(self):
        for value in (0.01, 0.5, 1.0, 5.0, 25.0, 100.0):
            self.assertAlmostEqual(inverse(forward(value)), value, places=3)

    def test_inverse_is_finite_at_saturation(self):
        # A channel the model hands back as exactly 1.0 must not become inf.
        self.assertTrue(0.0 < inverse(1.0) < 4096.0)

    def test_forward_is_monotonic(self):
        values = [forward(v) for v in (0.0, 0.1, 1.0, 10.0, 100.0)]
        self.assertEqual(values, sorted(values))


class BridgeStagingTests(unittest.TestCase):
    def setUp(self):
        self.source = BRIDGE.read_text(encoding="utf-8")

    def test_staging_uses_the_tonemap(self):
        self.assertIn("TonemapForward(src[x * 3 + 0])", self.source)
        self.assertIn("TonemapInverse(", self.source)

    def test_colour_is_not_clamped_into_0_1_on_the_way_in(self):
        # The regression this whole file exists to prevent.
        self.assertIsNone(
            re.search(r"LinearToSrgb\(std::clamp\(src\[", self.source),
            "colour is being clamped to 0..1 again before the model sees it",
        )

    def test_ceiling_matches_the_python_mirror(self):
        self.assertIn("1.0f - 1.0f / 2048.0f", self.source)


if __name__ == "__main__":
    unittest.main()
