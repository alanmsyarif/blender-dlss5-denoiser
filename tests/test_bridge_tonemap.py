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

# Mirror the bridge's constants: the knee below which colour passes through
# untouched, and the largest value FP16 holds below 1.0.
CEILING = 1.0 - 1.0 / 2048.0
KNEE_START = 0.8
KNEE_SCALE = 1.0


def forward(c):
    if c <= 0.0:
        return 0.0
    if c <= KNEE_START:
        return c
    over = c - KNEE_START
    return KNEE_START + (1.0 - KNEE_START) * (over / (over + KNEE_SCALE))


def inverse(c):
    if c <= 0.0:
        return 0.0
    if c <= KNEE_START:
        return c
    over = min((c - KNEE_START) / (1.0 - KNEE_START), CEILING)
    return KNEE_START + KNEE_SCALE * over / (1.0 - over)


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

    def test_below_the_knee_is_exact(self):
        # The point of the knee: the range most pixels live in is not resampled
        # at all, so it costs no precision to carry highlights.
        for value in (0.0, 0.1, 0.5, 0.79, KNEE_START):
            self.assertAlmostEqual(forward(value), value, places=6)
            self.assertAlmostEqual(inverse(forward(value)), value, places=6)

    def test_forward_is_monotonic(self):
        values = [forward(v) for v in (0.0, 0.1, 1.0, 10.0, 100.0)]
        self.assertEqual(values, sorted(values))


class BridgeStagingTests(unittest.TestCase):
    def setUp(self):
        self.source = BRIDGE.read_text(encoding="utf-8")

    def test_staging_uses_the_tonemap(self):
        self.assertIn("LinearToSrgb(TonemapForward(", self.source)
        self.assertIn("TonemapInverse(", self.source)

    def test_colour_is_not_clamped_into_0_1_on_the_way_in(self):
        # The regression this whole file exists to prevent. Matches the clamp
        # wherever the input comes from, so refactoring the encode loop cannot
        # quietly retire the guard the way an exact source anchor did.
        self.assertIsNone(
            re.search(r"LinearToSrgb\(\s*std::clamp\(", self.source),
            "colour is being clamped to 0..1 again before the model sees it",
        )

    def test_guided_path_is_exported(self):
        # The colour only export has to stay, because the Cycles side resolves
        # the guided one optionally and falls back when it is missing.
        self.assertIn("dlss5nr_process_guided", self.source)
        self.assertIn("__cdecl dlss5nr_process(", self.source)

    def test_reset_is_forced_without_guides(self):
        # Accumulating against cleared depth and motion is what produced the
        # 17% swing in output energy between renders.
        self.assertIn("!have_guides || reset || rebuilt", self.source)

    def test_constants_match_the_python_mirror(self):
        self.assertIn("1.0f - 1.0f / 2048.0f", self.source)
        self.assertIn("kKneeStart = 0.8f", self.source)
        self.assertIn("kKneeScale = 1.0f", self.source)


if __name__ == "__main__":
    unittest.main()
