import unittest

from tools.handheld.embedded_assets import symbols


class EmbeddedAssetSelectionTest(unittest.TestCase):
    def test_pal_and_ntsc_asset_names(self):
        pal = symbols("pal-1.0")
        ntsc = symbols("ntsc-1.0")
        self.assertIn("ovl_Boss_Ganon_00001E80_Tex", pal)
        self.assertNotIn("ovl_Boss_Ganon_00001E80_Tex", ntsc)
        self.assertIn("sTransWipeTex", pal & ntsc)
        self.assertIn("gCharPageHira", ntsc)
        self.assertNotIn("gCharPageHira", pal)


if __name__ == "__main__":
    unittest.main()
