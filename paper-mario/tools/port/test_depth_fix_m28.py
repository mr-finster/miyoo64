"""Host-only tests of the renderer's actual depth helper and state invalidation."""
import pathlib
import subprocess
import tempfile
import unittest
import re


ROOT = pathlib.Path(__file__).resolve().parents[2]


class DepthFixTests(unittest.TestCase):
    def test_state_derivation_and_transitions(self):
        source = (ROOT / 'bench/kit/gfx_pc.c').read_text()
        start = source.index('static inline bool pm_depth_compare(')
        end = source.index('\n}', start) + 2
        helper = source[start:end]
        start = source.index('static void gfx_sp_set_other_mode(')
        end = source.index('\n}', start) + 2
        update = source[start:end]
        self.assertIn('bool depth_test = pm_depth_compare(rsp.geometry_mode, rdp.other_mode_l);', source)
        self.assertIn('bool z_upd = (rdp.other_mode_l & Z_UPD) == Z_UPD;', source)
        gbi = (ROOT / 'refs/papermario/include/PR/gbi.h').read_text()
        definitions = []
        for name in ('G_ZBUFFER', 'Z_CMP', 'Z_UPD'):
            match = re.search(r'^\s*#\s*define\s+' + name + r'\s+(0x[0-9a-fA-F]+)', gbi, re.M)
            self.assertIsNotNone(match, name)
            definitions.append(f'#define {name} {match.group(1)}u\n')
        prefix = '#include <assert.h>\n#include <stdbool.h>\n#include <stdint.h>\n' + ''.join(definitions) + '''static struct { uint32_t other_mode_l, other_mode_h; } rdp;\nstatic struct { bool valid; } tri_state;\n'''
        main = '''
int main(void) {
    for (unsigned g=0;g<2;g++) for (unsigned c=0;c<2;c++) for (unsigned w=0;w<2;w++) {
        uint32_t other = (c ? Z_CMP : 0) | (w ? Z_UPD : 0);
        assert(pm_depth_compare(g ? G_ZBUFFER : 0, other) == (g && c));
        assert(((other & Z_UPD) == Z_UPD) == (w != 0));
    }
    tri_state.valid=true; gfx_sp_set_other_mode(4,1,Z_CMP);
    assert(!tri_state.valid && pm_depth_compare(G_ZBUFFER,rdp.other_mode_l));
    tri_state.valid=true; gfx_sp_set_other_mode(4,1,Z_CMP); assert(tri_state.valid);
    gfx_sp_set_other_mode(5,1,Z_UPD); assert(!tri_state.valid);
    tri_state.valid=true; gfx_sp_set_other_mode(4,1,0);
    assert(!tri_state.valid && !pm_depth_compare(G_ZBUFFER,rdp.other_mode_l) && (rdp.other_mode_l & Z_UPD));
    /* Captured CLOUD_NO_ZCMP and a Z-compared translucent material. */
    assert(!pm_depth_compare(G_ZBUFFER,0x0C184340u));
    assert(pm_depth_compare(G_ZBUFFER,0x0C184340u | Z_CMP));
    return 0;
}'''
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / 'test.c'
            path.write_text(prefix + helper + '\n' + update + '\n' + main)
            exe = pathlib.Path(tmp) / 'test'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', str(path), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_existing_backend_write_limit_is_explicit(self):
        soft = (ROOT / 'bench/kit/gfx_soft.c').read_text()
        self.assertIn('if (ZTEST && dmask) zb[idx] = (uint16_t)uz;', soft)
        self.assertIn('const bool ztest = use_z && sDepthTest;', soft)


if __name__ == '__main__':
    unittest.main()
