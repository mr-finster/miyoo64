"""ROM-free planning tests; no compiler or ROM access."""
import contextlib
import io
import json
import unittest
import pal_build

class PalBuildPlanTests(unittest.TestCase):
    def test_plan_never_opens_a_rom_or_invokes_tools(self):
        out=io.StringIO()
        with contextlib.redirect_stdout(out):self.assertEqual(pal_build.main(['--plan','--rom','missing-own-rom.z64']),0)
        data=json.loads(out.getvalue());text=json.dumps(data)
        self.assertIn('build/game/pal/',text)
        self.assertNotIn('recipes_handheld.json',text)
        self.assertNotIn('recipes_miyoo.json',text)
    def test_both_generators_select_pal(self):
        jobs=pal_build.commands('own.z64')
        generators=[x for x in jobs if 'tools/port/gen_build.py' in x]
        self.assertEqual(len(generators),2)
        for command in generators:self.assertEqual(command[-2:],['--region','pal'])
    def test_fresh_cache_is_scoped_to_build_and_identity(self):
        a=pal_build.commands('own.z64','00000001')[-1]
        b=pal_build.commands('own.z64','00000002')[-1]
        self.assertNotEqual(a[a.index('--cache')+1],b[b.index('--cache')+1])
        self.assertTrue(a[a.index('--cache')+1].startswith('build/game/pal/miyoo/'))
    def test_overlay_flag_changes_cannot_reuse_stale_game_objects(self):
        make=next(x for x in pal_build.commands('own.z64') if x[0]=='make')
        self.assertIn('-B',make)
    def test_every_product_path_belongs_to_pal(self):
        for command in pal_build.commands('own.z64'):
            for token in command:
                if 'build/game/' in token:self.assertIn('build/game/pal/',token)
if __name__=='__main__':unittest.main()
