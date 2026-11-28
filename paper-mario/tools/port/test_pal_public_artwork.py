"""The public PAL package must use the existing US artwork only."""
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]

class PublicArtworkTests(unittest.TestCase):
    def test_public_manifest_uses_us_icon_and_default_us_cover(self):
        pal = json.loads((ROOT / 'miyoo/game-pal-1.0.json').read_text())
        us = json.loads((ROOT / 'miyoo/game.json').read_text())
        self.assertEqual(pal['icon'], us['icon'])
        self.assertNotIn('cover', pal)
        self.assertNotIn('cover', us)
        self.assertFalse((ROOT / 'miyoo/cover-pal.png').exists())
        self.assertFalse((ROOT / 'miyoo/icon-pal.png').exists())

if __name__ == '__main__':
    unittest.main()
