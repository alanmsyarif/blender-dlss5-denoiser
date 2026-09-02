import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "patches/blender-v5.0.1-dlss5nr.patch"


class PatchsetTests(unittest.TestCase):
    def test_patch_contains_native_cycles_integration(self):
        text = PATCH.read_text(encoding="utf-8")
        for marker in (
            "WITH_CYCLES_DLSS5_NR",
            "DENOISER_DLSS5NR",
            "denoiser_dlss5nr.cpp",
            "with_dlss5nr",
            "DLSS 5 NR (Experimental)",
        ):
            self.assertIn(marker, text)

    def test_runtime_binary_is_not_bundled(self):
        forbidden = {"nvngx_dlssnr.dll", "_nvngx.dll"}
        found = [p for p in ROOT.rglob("*") if p.is_file() and p.name.lower() in forbidden]
        self.assertEqual(found, [])

    def test_exact_blender_revision_is_pinned(self):
        expected = "a3db93c5b2595a79f65f304114c23aeef8c2139f"
        for script in ("fetch_blender.ps1", "apply_patch.ps1"):
            self.assertIn(expected, (ROOT / "scripts" / script).read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
