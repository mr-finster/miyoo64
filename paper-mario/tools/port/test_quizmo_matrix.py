"""The native Quizmo stage matrices must preserve the N64's 32-bit words."""
import re
import struct
import unittest
from pathlib import Path


class QuizmoMatrixTest(unittest.TestCase):
    def test_matrix_words_match_original_big_endian_bytes(self):
        patch = (Path(__file__).resolve().parents[2] / 'game/patches/decomp.patch').read_text()
        section = patch.split('diff --git a/src/effects/gfx/quizmo_stage.c ', 1)[1].split('\ndiff --git ', 1)[0]
        old = section.split('-char D_09004148_39CD08[] = {', 1)[1].split('\n+/* Four N64 matrices', 1)[0]
        raw = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]+)', old))
        new = section.split('+Mtx D_09004148_39CD08[4] = {', 1)[1].split('\n };', 1)[0]
        rows = re.findall(r'^\+\s*\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\},', new, re.M)
        self.assertEqual(len(raw), 4 * 64)
        self.assertEqual(len(rows), 4 * 4)
        words = [int(n) for row in rows for n in row]
        self.assertEqual(struct.pack('>64i', *words), raw)
        native = struct.pack('<64i', *words)
        self.assertEqual(native, b''.join(raw[i:i+4][::-1] for i in range(0, len(raw), 4)))


if __name__ == '__main__':
    unittest.main()
