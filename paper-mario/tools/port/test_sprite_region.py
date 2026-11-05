"""Check the real decomp patch's two sprite return paths in both regions."""
from pathlib import Path
import re
import subprocess
import unittest


class SpriteRegionTests(unittest.TestCase):
    def test_sprite_return_paths_are_region_specific(self):
        patch = Path(__file__).resolve().parents[2] / 'game/patches/decomp.patch'
        text = patch.read_text(encoding='utf-8')
        section = text.split('diff --git a/src/sprite.c b/src/sprite.c\n', 1)[1].split('diff --git ', 1)[0]
        lines = []
        for line in section.splitlines():
            if line.startswith('+++') or line.startswith('---'):
                continue
            if line.startswith(('+', ' ')):
                lines.append(line[1:])
        source = '\n'.join(lines) + '\n'
        for region, bare, minus_one in ((0, 2, 1), (1, 0, 3)):
            with self.subTest(version_pal=region):
                output = subprocess.run(['cpp', '-P', '-DVERSION_PAL=' + str(region)],
                                        input=source, text=True, capture_output=True, check=True).stdout
                self.assertEqual(len(re.findall(r'\breturn\s*;', output)), bare)
                self.assertEqual(len(re.findall(r'\breturn\s+-1\s*;', output)), minus_one)


if __name__ == '__main__':
    unittest.main()
