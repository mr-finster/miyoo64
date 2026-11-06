"""Preprocess the actual region branches for US/PAL ROM messages."""
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / 'game/port'


class USMessageCompatibilityTests(unittest.TestCase):
    def preprocess(self, filename, pal):
        text = (PORT / filename).read_text(encoding='utf-8')
        blocks = re.findall(r'^#if defined\(PM64_ROM_REGION_PAL\)\n.*?^#else\n.*?^#endif',
                            text, flags=re.MULTILINE | re.DOTALL)
        self.assertTrue(blocks)
        source = '#include "port_rom_identity.h"\n' + '\n'.join(blocks) + '\n'
        cmd = ['cpp', '-P', '-I', str(PORT)]
        if pal:
            cmd.append('-DPM64_ROM_REGION_PAL=1')
        return subprocess.run(cmd, input=source, text=True, capture_output=True, check=True).stdout

    def test_us_messages_and_pinned_identity(self):
        a = self.preprocess('port_assets_hh.c', False)
        h = self.preprocess('port_host.c', False)
        r = self.preprocess('port_rom.c', False)
        self.assertIn('memcmp(hdr + 0x3B, "NMQE", 4)', a)
        self.assertIn('not the US (NTSC) version (code %.4s)', a)
        self.assertIn('rom: %s (Paper Mario (USA), SHA-1 checked)', a)
        self.assertIn('It must be the US (NTSC) cart.', a)
        self.assertIn('port: no Paper Mario (USA) ROM found%s%s', h)
        self.assertIn('not Paper Mario (USA)', r)
        self.assertNotIn('Paper Mario (Europe, PAL)', a + h + r)

    def test_pal_messages_remain_region_specific(self):
        a = self.preprocess('port_assets_hh.c', True)
        h = self.preprocess('port_host.c', True)
        r = self.preprocess('port_rom.c', True)
        self.assertIn('memcmp(hdr + 0x3B, "NMQP", 4)', a)
        self.assertIn('not the PAL (Europe) version', a)
        self.assertIn('It must be the European (PAL) cart\'s dump.', a)
        self.assertIn('Paper Mario (Europe, PAL)', a + h + r)
        self.assertNotIn('It must be the US (NTSC) cart.', a)


if __name__ == '__main__':
    unittest.main()
