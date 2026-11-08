"""Synthetic native-generator tests; never build or run any game code."""
from contextlib import redirect_stderr, redirect_stdout
import errno
import hashlib
import io
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import gen_build

# Frozen from the unmodified upstream 7e740e9 generator with the same fixture.
# Overlay boundaries and .data/.bss flags are part of the current US contract.
US_BASELINES = {'pc': {'rom_syms.ld': 'cbd4c658a5ac2b18fe92c0e75bdf2196ad8e5da8ce98fb4004fa780746bb5c43', 'port_segtab.c': '9fde70fb2dd7eef0d9a80a35f6f36798326108e48784c47410e850640468bf57', 'pc.ninja': 'e59339c6f25e33a143f674fa51a5c0f0163fc64751d44bbf2e533e7d765c167a', 'pc_game.mk': 'f79b71fc8e2495551d49545d74ee742d8305a45eaf39184179e202ffdbc7a941', 'dontsave.ld': '99defe703ca746070439f5fba3c80fdb92301cdc3aff195b650f3fade2f9abdf', 'assets_progbits.ld': '41aa6d90f63c8a788a13c25db196016f2161504c21f72de323efad4081de9f37', 'assets_noload.ld': '47f6a4d095b0a96aa4328b5e7a9358da1595047bd2fcdef2122a266e74715b30', 'port_effects.c': '0ec6e74eef418f31c5f9254ea4a0c61600a3df0edadd021525410ea96582f1b7', 'segments.json': '4030891c07f6bce2d883f0515a6b53865f0ed2f98c24b0bb2c88cf7e63e5d7ba', 'pc/build_stamp.c': '995877cb1c40de899bc2a20b8f56e3dfa972610d6ba9d4910721acf91fe4a57d'}, 'handheld': {'rom_syms.ld': 'cbd4c658a5ac2b18fe92c0e75bdf2196ad8e5da8ce98fb4004fa780746bb5c43', 'port_segtab.c': '9fde70fb2dd7eef0d9a80a35f6f36798326108e48784c47410e850640468bf57', 'handheld.ninja': 'fdfde5cd03ad43c0ff6382dfc9c1760487186eb009cd106f9b463507165f4bc2', 'handheld_game.mk': '549f79a00042db8a5ea81940114e5038e2ff153384da211031bef1aa536b35c9', 'dontsave.ld': '99defe703ca746070439f5fba3c80fdb92301cdc3aff195b650f3fade2f9abdf', 'assets_progbits.ld': '41aa6d90f63c8a788a13c25db196016f2161504c21f72de323efad4081de9f37', 'assets_noload.ld': '47f6a4d095b0a96aa4328b5e7a9358da1595047bd2fcdef2122a266e74715b30', 'port_effects.c': '0ec6e74eef418f31c5f9254ea4a0c61600a3df0edadd021525410ea96582f1b7', 'segments.json': '4030891c07f6bce2d883f0515a6b53865f0ed2f98c24b0bb2c88cf7e63e5d7ba', 'handheld/build_stamp.c': '995877cb1c40de899bc2a20b8f56e3dfa972610d6ba9d4910721acf91fe4a57d'}, 'miyoo': {'rom_syms.ld': 'cbd4c658a5ac2b18fe92c0e75bdf2196ad8e5da8ce98fb4004fa780746bb5c43', 'port_segtab.c': '9fde70fb2dd7eef0d9a80a35f6f36798326108e48784c47410e850640468bf57', 'miyoo.ninja': '42add5603473c07dd0f111ce60925ea51b3b6c565f750b88c8a26bbad72f7b8c', 'dontsave.ld': '99defe703ca746070439f5fba3c80fdb92301cdc3aff195b650f3fade2f9abdf', 'assets_progbits.ld': '41aa6d90f63c8a788a13c25db196016f2161504c21f72de323efad4081de9f37', 'assets_noload.ld': '47f6a4d095b0a96aa4328b5e7a9358da1595047bd2fcdef2122a266e74715b30', 'port_effects.c': '0ec6e74eef418f31c5f9254ea4a0c61600a3df0edadd021525410ea96582f1b7', 'segments.json': '4030891c07f6bce2d883f0515a6b53865f0ed2f98c24b0bb2c88cf7e63e5d7ba', 'miyoo/build_stamp.c': '995877cb1c40de899bc2a20b8f56e3dfa972610d6ba9d4910721acf91fe4a57d'}}

class BuildGeneratorTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory(prefix='pal_build_test_')))
        self.script = self.root / 'tools/port/gen_build.py'
        self.script.parent.mkdir(parents=True)
        shutil.copyfile(Path(gen_build.__file__), self.script)
        self.decomp = self.root / 'refs/papermario'
        self.build = self.root / 'build/game'
        self.make_fixture()

    def write(self, name, text):
        target = self.root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding='utf-8')
        return target

    def make_fixture(self):
        for filename in ('game/port/port_host.c', 'game/port/port_test.c',
                         'bench/kit/gfx_pc.c', 'bench/kit/gfx_cc.c', 'bench/kit/gfx_soft.c'):
            self.write(filename, '/* synthetic source */\n')
        for filename in ('savestate', 'miyoo_panel', 'miyoo_menu', 'miyoo_remap',
                         'miyoo_worker', 'miyoo_clock', 'miyoo_sleepsave'):
            self.write(f'kit/platform/{filename}.c', '/* synthetic source */\n')
        self.write('refs/papermario/src/effect_shims.yaml', '- shim_test_fn\n- shim_NULL\n')
        ninja = []
        for region in ('us', 'pal'):
            sources = [('main', 'src/main.c', 'cc'), ('font', f'assets/{region}/font.c', 'cc'),
                       ('map', 'src/world/map.c', 'cc'), ('boot', 'src/boot/entry.s', 'as'),
                       ('messages', f'assets/{region}/msg.bin', 'bin')]
            ld = []
            for segment, source, rule in sources:
                obj = f'ver/{region}/build/{source}.o'
                address = {'main': '0x80025C00', 'font': '0x00000000', 'map': '0x80240000',
                           'boot': '0xA4000040', 'messages': '0x00000000'}[segment]
                ld.extend([f'    .{segment} {address} :', f'        {obj}(.text);'])
                if segment == 'main':
                    ld.extend(['    .main_bss (NOLOAD) :', f'        {obj}(.bss);'])
                ninja.extend([f'build {obj}: {rule} {source}', f'  cppflags = -DVERSION_{region.upper()}',
                              '  cflags = -synthetic'])
                self.write('refs/papermario/' + source, '/* synthetic */\n')
            for header in ('ld_addrs.h', 'message_ids.h', 'synthetic.inc.h'):
                self.write(f'refs/papermario/ver/{region}/build/include/{header}', '/* synthetic header */\n')
            ninja.extend([f'build generated_code_{region}: phony ver/{region}/build/include/synthetic.inc.h',
                          f'build inc_img_bins_{region}: phony'])
            if region == 'pal':
                self.write('refs/papermario/ver/pal/splat.yaml',
                           'segments:\n'
                           '  - name: main\n    type: code\n    start: 0x0\n    vram: 0x80025C00\n'
                           '  - name: font\n    type: code\n    start: 0x10\n    vram: 0\n'
                           '    subsegments:\n      - type: .data\n        name: font\n'
                           '  - name: map\n    type: code\n    start: 0x20\n    vram: 0x80240000\n')
            self.write(f'refs/papermario/ver/{region}/papermario.ld', '\n'.join(ld) + '\n')
            self.write(f'refs/papermario/ver/{region}/undefined_syms.txt',
                       f'{region}_only_symbol = 0x1234;\ngBackgroundImage = 0x80200000;\n')
            self.write(f'refs/papermario/assets/{region}/effects/effect_macros.h',
                       '#define EFFECT_DEF_TEST(func_name) void func_name(float)\n')
            index = '01' if region == 'us' else '02'
            self.write(f'refs/papermario/assets/{region}/effects/effect_defs.h',
                       f'    EFFECT_TEST = 0x{index},\nEFFECT_DEF_TEST(fx_test);\n')
            names = ['main', 'font', 'map', 'boot', 'messages']
            if region == 'pal':
                names += ['msg_pal_en', 'msg_pal_de', 'msg_pal_fr', 'msg_pal_es']
            ranges = [{'name': name, 'type': 'code', 'rom_start': i * 16, 'rom_end': (i + 1) * 16}
                      for i, name in enumerate(names)]
            ranges.append({'name': None, 'type': None, 'rom_start': len(names) * 16, 'rom_end': None})
            prefix = 'build/game' + ('/pal' if region == 'pal' else '')
            self.write(prefix + '/rom_segments.json', json.dumps(ranges, indent=0))
        self.write('refs/papermario/build.ninja', '\n'.join(ninja) + '\n')

    def cli(self, *args, extra_env=None):
        environment = dict(os.environ, PM64_SAN='', MIYOO_TOOLCHAIN='/opt/miyoomini-toolchain')
        environment.update(extra_env or {})
        return subprocess.run([sys.executable, '-B', str(self.script), *args], cwd=self.root,
                              env=environment, text=True, capture_output=True, timeout=10)

    def snapshot(self, region='us'):
        base = self.build / 'pal' if region == 'pal' else self.build
        return {str(path.relative_to(base)): path.read_bytes() for path in base.rglob('*')
                if path.is_file() and (region == 'pal' or 'pal' not in path.relative_to(base).parts)}

    @staticmethod
    def code_ranges(data):
        # Follow-address tests check ROM range identity. Overlay-bound pointers
        # now also depend on classification; dedicated overlay tests cover them.
        return re.findall(rb'\{ (0x[0-9A-F]+), (0x[0-9A-F]+), "([^"]+)"', data)

    def generated(self, region='us'):
        return {name: data for name, data in self.snapshot(region).items() if name != 'rom_segments.json'}

    def hashes(self):
        return {name: hashlib.sha256(data.replace(str(self.root).encode(), b'<ROOT>')).hexdigest()
                for name, data in self.generated().items()}

    def test_us_default_matches_frozen_original(self):
        result = self.cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.hashes(), US_BASELINES['pc'])

    def test_us_all_platforms_match_frozen_original(self):
        for platform in ('pc', 'handheld', 'miyoo'):
            with self.subTest(platform=platform):
                # Remove only synthetic generated output from this test's temporary tree.
                for name in self.generated():
                    (self.build / name).unlink()
                result = self.cli(platform)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.hashes(), US_BASELINES[platform])

    def test_pal_host_rom_identity_define_is_present_on_all_platforms(self):
        for platform in ('pc', 'handheld', 'miyoo'):
            with self.subTest(platform=platform):
                result = self.cli(platform, '--region', 'pal')
                self.assertEqual(result.returncode, 0, result.stderr)
                ninja = (self.build / f'pal/{platform}.ninja').read_text()
                self.assertIn(' -DPM64_ROM_REGION_PAL=1', ninja)
                host = ninja.split('rule cc_host\n', 1)[1].split('rule ', 1)[0]
                self.assertIn('-DPM64_ROM_REGION_PAL=1', host)
                game = ninja.split('rule cc_game_port\n', 1)[1].split('rule ', 1)[0]
                self.assertNotIn('-DPM64_ROM_REGION_PAL', game)

    def test_us_host_rom_identity_define_is_absent_on_all_platforms(self):
        for platform in ('pc', 'handheld', 'miyoo'):
            with self.subTest(platform=platform):
                result = self.cli(platform, '--region', 'us')
                self.assertEqual(result.returncode, 0, result.stderr)
                ninja = (self.build / f'{platform}.ninja').read_text()
                self.assertNotIn('-DPM64_ROM_REGION_PAL', ninja)

    def test_explicit_us_is_identical_to_default(self):
        self.assertEqual(self.cli().returncode, 0)
        before = self.generated()
        result = self.cli('--region', 'us')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.generated(), before)

    def test_pal_all_platforms_inputs_defines_and_output_isolation(self):
        self.assertEqual(self.cli('handheld').returncode, 0)
        self.assertEqual(self.cli('miyoo').returncode, 0)
        us = self.snapshot()
        for platform in ('pc', 'handheld', 'miyoo'):
            result = self.cli(platform, '--region', 'pal')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(self.snapshot(), us)
            ninja = (self.build / 'pal' / f'{platform}.ninja').read_text()
            for text in ('-DVERSION=pal -DVERSION_PAL', '/ver/pal/build/include', '/assets/pal', '/build/game/pal/'):
                self.assertIn(text, ninja)
            for text in ('VERSION_US', '/ver/us/', '/assets/us/'):
                self.assertNotIn(text, ninja)
            self.assertIn('builddir = /work/build/game/pal', ninja)
            self.assertIn('/build/game/pal/handheld/src/main.c.o' if platform == 'miyoo'
                          else f'/build/game/pal/{platform}/src/main.c.o', ninja)
            if platform != 'miyoo':
                make = (self.build / 'pal' / f'{platform}_game.mk').read_text()
                self.assertIn('/build/game/pal/port_effects.c', make)
                self.assertIn('-DVERSION_PAL', make)
                self.assertNotIn('/ver/us/', make)
        self.assertIn('load_effect(0x02)', (self.build / 'pal/port_effects.c').read_text())
        symbols = (self.build / 'pal/rom_syms.ld').read_text()
        self.assertIn('pal_only_symbol', symbols)
        self.assertNotIn('us_only_symbol', symbols)
        self.assertIn('msg_pal_de_ROM_START', symbols)
        self.assertIn('font_ROM_START = 0', symbols)

    def test_selected_region_ignores_other_regions_missing_inputs(self):
        for path in (self.decomp / 'ver/us/papermario.ld', self.decomp / 'assets/us/effects/effect_defs.h'):
            path.unlink()
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_us_ignores_missing_pal_inputs(self):
        (self.decomp / 'ver/pal/papermario.ld').unlink()
        result = self.cli()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_missing_prerequisites_leave_existing_outputs_unchanged(self):
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)
        before = self.snapshot('pal')
        names = ['ver/pal/papermario.ld', 'build.ninja', 'ver/pal/undefined_syms.txt',
                 'ver/pal/build/include/ld_addrs.h', 'ver/pal/build/include/message_ids.h',
                 'ver/pal/build/include/synthetic.inc.h', 'assets/pal/effects/effect_macros.h',
                 'assets/pal/effects/effect_defs.h', 'assets/pal/font.c', 'src/effect_shims.yaml']
        for name in names:
            with self.subTest(name=name):
                target = self.decomp / name
                content = target.read_bytes()
                target.unlink()
                result = self.cli('--region', 'pal')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('missing', result.stderr)
                self.assertNotIn('Traceback', result.stderr)
                self.assertEqual(self.snapshot('pal'), before)
                target.write_bytes(content)

    def test_missing_pal_segment_table_does_not_fall_back_to_us(self):
        (self.build / 'pal/rom_segments.json').unlink()
        us = self.snapshot()
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('rom_segments.py --region pal', result.stderr)
        self.assertEqual(self.generated('pal'), {})
        self.assertEqual(self.snapshot(), us)

    def test_unknown_region_or_platform_has_no_outputs(self):
        for args in (('--region', 'jp'), ('unknown',)):
            with self.subTest(args=args):
                result = self.cli(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('invalid choice', result.stderr)
                self.assertEqual(self.generated(), {})
                self.assertEqual(self.generated('pal'), {})

    def test_mixed_region_flags_rejected(self):
        path = self.decomp / 'build.ninja'
        path.write_text(path.read_text().replace('  cppflags = -DVERSION_PAL', '  cppflags = -DVERSION_US'))
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('wrong region', result.stderr)
        self.assertEqual(self.generated('pal'), {})

    def test_missing_object_rule_rejected(self):
        path = self.decomp / 'build.ninja'
        path.write_text(path.read_text().replace('build ver/pal/build/src/main.c.o:', 'build ver/pal/build/src/other.c.o:'))
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('no build.ninja object rule', result.stderr)
        self.assertEqual(self.generated('pal'), {})

    def test_empty_pal_units_rejected(self):
        (self.decomp / 'ver/pal/papermario.ld').write_text('    .main 0x80025C00 :\n')
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('no native C units', result.stderr)

    def test_missing_pal_phony_targets_rejected(self):
        path = self.decomp / 'build.ninja'
        path.write_text(path.read_text().replace('build generated_code_pal:', 'build wrong_generated_target:'))
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('missing PAL prerequisite targets', result.stderr)

    def test_wrong_pal_segment_table_rejected(self):
        (self.build / 'pal/rom_segments.json').write_bytes((self.build / 'rom_segments.json').read_bytes())
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('four msg_pal', result.stderr)

    def test_invalid_segment_bounds_rejected(self):
        target = self.build / 'pal/rom_segments.json'
        table = json.loads(target.read_text())
        table[0]['rom_end'] = -1
        target.write_text(json.dumps(table))
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('bounds', result.stderr)

    def test_missing_native_segment_range_rejected(self):
        target = self.build / 'pal/rom_segments.json'
        table = json.loads(target.read_text())
        table[0]['name'] = 'not_main'
        target.write_text(json.dumps(table))
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('missing ROM ranges', result.stderr)

    def test_proven_follow_preserves_rom_symbols_and_isolation(self):
        for region in ('us', 'pal'):
            with self.subTest(region=region):
                result = self.cli('--region', region)
                self.assertEqual(result.returncode, 0, result.stderr)
                before = self.generated(region)
                other = self.snapshot('pal' if region == 'us' else 'us')
                path = self.decomp / f'ver/{region}/papermario.ld'
                text = path.read_text()
                obj = f'ver/{region}/build/assets/{region}/font.c.o'
                text = text.replace(f'    .font 0x00000000 :\n        {obj}(.text);',
                    f'    .font 0x00000000 : AT(font_ROM_START) SUBALIGN(8)\n'
                    f'    {{\n        {obj}(.text);\n    }}\n'
                    '    . = ALIGN(., 16);\n    font_VRAM_END = .;')
                path.write_text(text.replace('.map 0x80240000 :', '.map font_VRAM_END :'))
                result = self.cli('--region', region)
                self.assertEqual(result.returncode, 0, result.stderr)
                after = self.generated(region)
                for name in ('rom_syms.ld', 'port_segtab.c', 'pc.ninja'):
                    if name == 'rom_syms.ld':
                        self.assertEqual(after[name], before[name].replace(
                            b'PROVIDE(map_VRAM = 0x80240000);\n', b''))
                    else:
                        self.assertEqual(self.code_ranges(after[name]), self.code_ranges(before[name])) if name == 'port_segtab.c' else self.assertEqual(after[name], before[name])
                self.assertEqual(self.snapshot('pal' if region == 'us' else 'us'), other)
                snapshot = self.snapshot(region)
                path.write_text(path.read_text().replace('font_VRAM_END = .;',
                                                       'font_VRAM_END = .;\n    . += 16;'))
                result = self.cli('--region', region)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.snapshot(region), snapshot)

    def test_prerequisite_paths_normalize_without_intermediate_directories(self):
        for region in ('us', 'pal'):
            with self.subTest(region=region):
                self.assertEqual(self.cli('--region', region).returncode, 0)
                before = self.snapshot(region)
                other = self.snapshot('pal' if region == 'us' else 'us')
                ninja = self.decomp / 'build.ninja'
                original = ninja.read_text()
                canonical = f'ver/{region}/build/include/synthetic.inc.h'
                alias = f'ver/{region}/build/include/absent/nested/../.././synthetic.inc.h'
                self.assertFalse((self.decomp / f'ver/{region}/build/include/absent').exists())
                ninja.write_text(original.replace(canonical, alias))
                result = self.cli('--region', region)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.snapshot(region), before)
                self.assertEqual(self.snapshot('pal' if region == 'us' else 'us'), other)
                ninja.write_text(original)

    def test_source_paths_are_canonical_in_all_native_build_descriptions(self):
        ninja = self.decomp / 'build.ninja'
        original = ninja.read_text()
        for region in ('us', 'pal'):
            for platform in ('pc', 'handheld', 'miyoo'):
                with self.subTest(region=region, platform=platform):
                    ninja.write_text(original)
                    self.assertEqual(self.cli(platform, '--region', region).returncode, 0)
                    before = self.snapshot(region)
                    for alias in ('src/absent/.././main.c',
                                  '/work/refs/papermario/src/absent/../main.c',
                                  str(self.decomp / 'src/absent/../main.c')):
                        with self.subTest(alias=alias):
                            ninja.write_text(original.replace(
                                f'build ver/{region}/build/src/main.c.o: cc src/main.c',
                                f'build ver/{region}/build/src/main.c.o: cc {alias}'))
                            result = self.cli(platform, '--region', region)
                            self.assertEqual(result.returncode, 0, result.stderr)
                            self.assertEqual(self.snapshot(region), before)
        ninja.write_text(original)

    def test_invalid_input_paths_preserve_outputs_and_region_isolation(self):
        ninja = self.decomp / 'build.ninja'
        original = ninja.read_text()
        inside = self.decomp / 'different/subdirectory'
        inside.mkdir(parents=True)
        (self.decomp / 'different/main.c').write_text('/* other synthetic source */')
        (self.decomp / 'src/redirect').symlink_to('../different/subdirectory', target_is_directory=True)
        outside = self.write('outside.h', '/* external synthetic input */')
        (self.decomp / 'src/escape').symlink_to(outside)
        for region in ('us', 'pal'):
            other_region = 'pal' if region == 'us' else 'us'
            header = f'ver/{region}/build/include/synthetic.inc.h'
            cross = self.decomp / f'ver/{region}/build/include/cross.h'
            cross.symlink_to(self.decomp / f'ver/{other_region}/build/include/synthetic.inc.h')
            cases = [('source escape', 'src/../../outside.h', True),
                     ('source foreign absolute', str(outside), True),
                     ('source symlink cancellation', 'src/redirect/../main.c', True),
                     ('source external symlink', 'src/escape', True),
                     ('prerequisite escape', '../outside.h', False),
                     ('prerequisite foreign absolute', str(outside), False),
                     ('prerequisite symlink cancellation', 'src/redirect/../main.c', False),
                     ('prerequisite hidden other region',
                      f'ver/{region}/../{other_region}/build/include/synthetic.inc.h', False),
                     ('prerequisite symlink other region', str(cross), False)]
            for label, path, source in cases:
                for existing in (True, False):
                    with self.subTest(region=region, case=label, existing=existing):
                        ninja.write_text(original)
                        if existing:
                            self.assertEqual(self.cli('--region', region).returncode, 0)
                        else:
                            destination = self.build / 'pal' if region == 'pal' else self.build
                            for filename in self.generated(region):
                                (destination / filename).unlink()
                        before, other = self.snapshot(region), self.snapshot(other_region)
                        if source:
                            invalid = original.replace(
                                f'build ver/{region}/build/src/main.c.o: cc src/main.c',
                                f'build ver/{region}/build/src/main.c.o: cc {path}')
                        else:
                            invalid = original.replace(header, path)
                        ninja.write_text(invalid)
                        result = self.cli('--region', region)
                        self.assertNotEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(self.snapshot(region), before)
                        self.assertEqual(self.snapshot(other_region), other)
                        if not existing:
                            self.assertEqual(self.generated(region), {})
        ninja.write_text(original)

    def test_metadata_failures_abort_before_publication(self):
        ninja = self.decomp / 'build.ninja'
        original = ninja.read_text()
        real_lstat = Path.lstat
        for region in ('us', 'pal'):
            fault = self.decomp / f'ver/{region}/build/blocked/link'
            fault.parent.mkdir(parents=True, exist_ok=True)
            (fault.parent.parent / 'input.h').write_text('/* synthetic */')
            alias = f'ver/{region}/build/blocked/link/../../input.h'
            for source in (False, True):
                for code in (errno.EACCES, errno.EPERM, errno.EIO, errno.ENOTDIR, errno.ENOMEM, None):
                    for existing in (False, True):
                        with self.subTest(region=region, source=source, errno=code, existing=existing):
                            ninja.write_text(original)
                            if existing:
                                self.assertEqual(self.cli('--region', region).returncode, 0)
                            else:
                                destination = self.build / 'pal' if region == 'pal' else self.build
                                for filename in self.generated(region):
                                    (destination / filename).unlink()
                            before = self.snapshot(region)
                            other_region = 'pal' if region == 'us' else 'us'
                            other = self.snapshot(other_region)
                            if source:
                                text = original.replace(
                                    f'build ver/{region}/build/src/main.c.o: cc src/main.c',
                                    f'build ver/{region}/build/src/main.c.o: cc {alias}')
                            else:
                                text = original.replace(f'ver/{region}/build/include/synthetic.inc.h', alias)
                            ninja.write_text(text)
                            failure = OSError(code, 'synthetic metadata failure', str(fault))

                            def fail_lstat(path, *args, **kwargs):
                                if path == fault:
                                    raise failure
                                return real_lstat(path, *args, **kwargs)

                            stderr, stdout = io.StringIO(), io.StringIO()
                            with patch.multiple(gen_build, ROOT=str(self.root), DECOMP=str(self.decomp)), \
                                    patch.object(Path, 'lstat', fail_lstat), \
                                    redirect_stdout(stdout), redirect_stderr(stderr):
                                with self.assertRaises(SystemExit) as stopped:
                                    gen_build.main(['--region', region])
                            self.assertEqual(stopped.exception.code, 1)
                            self.assertIn('cannot inspect PMRET input component', stderr.getvalue())
                            self.assertIn(str(fault), stderr.getvalue())
                            self.assertNotIn('gen_build.py: wrote', stdout.getvalue())
                            self.assertEqual(self.snapshot(region), before)
                            self.assertEqual(self.snapshot(other_region), other)
                            destination = self.build / 'pal' if region == 'pal' else self.build
                            self.assertEqual(list(destination.glob('.gen-build-*')), [])
        ninja.write_text(original)

    def add_synthetic_palette_pairs(self, count=1):
        table_path = self.build / 'pal/rom_segments.json'
        table = json.loads(table_path.read_text())
        terminal = table.pop()
        start = terminal['rom_start']
        blocks, rules, pairs = [], [], []
        for index in range(count):
            raw, palette_name = f'graphic_{index}', f'colors_{index}'
            entries = [{'name': raw, 'type': 'ci8', 'rom_start': start, 'rom_end': start + 16},
                       {'name': raw, 'type': 'palette', 'rom_start': start + 16, 'rom_end': start + 24}]
            table.extend(entries)
            for name, extension, size in ((raw, 'png', 16), (palette_name, 'pal', 8)):
                stem = f'ver/pal/build/assets/pal/synthetic/texture_{index}.{extension}'
                self.write('refs/papermario/' + stem + '.bin', 'x' * size)
                rules.append(f'build {stem}.o: bin {stem}.bin')
                blocks.append(self.synthetic_data_section(name, stem + '.o'))
            pairs.append((raw, palette_name, entries))
            start += 24
        table.append(dict(terminal, rom_start=start))
        table_path.write_text(json.dumps(table))
        ld = self.decomp / 'ver/pal/papermario.ld'
        ld.write_text(ld.read_text() + '\n'.join(blocks))
        ninja = self.decomp / 'build.ninja'
        ninja.write_text(ninja.read_text() + '\n'.join(rules) + '\n')
        return pairs

    @staticmethod
    def synthetic_data_section(name, obj):
        return (f'    {name}_ROM_START = __romPos;\n'
                f'    {name}_VRAM = ADDR(.{name});\n'
                f'    .{name} : AT({name}_ROM_START) SUBALIGN(8)\n'
                '    {\n        FILL(0x00000000);\n'
                f'        {name}_DATA_START = .;\n        {obj}(.data);\n'
                f'        {name}_DATA_END = .;\n'
                f'        {name}_DATA_SIZE = ABSOLUTE({name}_DATA_END - {name}_DATA_START);\n'
                '    }\n'
                f'    __romPos += SIZEOF(.{name});\n'
                f'    {name}_ROM_END = __romPos;\n    {name}_VRAM_END = .;\n')

    def test_palette_identity_uses_proven_linker_names_on_all_platforms(self):
        pairs = self.add_synthetic_palette_pairs(3)
        table_path = self.build / 'pal/rom_segments.json'
        raw_bytes = table_path.read_bytes()
        original = json.loads(raw_bytes)
        with patch.multiple(gen_build, ROOT=str(self.root), DECOMP=str(self.decomp)):
            segs = gen_build.parse_ld(self.decomp / 'ver/pal/papermario.ld', 'pal')
            resolved, inputs = gen_build.pal_rom_identities(original, segs, self.decomp / 'build.ninja')
        self.assertEqual(len(resolved), len(original))
        self.assertEqual(len(inputs), 6)
        self.assertEqual([e['rom_start'] for e in resolved], [e['rom_start'] for e in original])
        self.assertEqual([e['rom_end'] for e in resolved], [e['rom_end'] for e in original])
        self.assertEqual([e['type'] for e in resolved], [e['type'] for e in original])
        self.assertEqual(original, json.loads(raw_bytes))
        self.assertEqual(self.cli('--region', 'us').returncode, 0)
        us = self.snapshot('us')
        for platform in ('pc', 'handheld', 'miyoo'):
            result = self.cli(platform, '--region', 'pal')
            self.assertEqual(result.returncode, 0, result.stderr)
            symbols = (self.build / 'pal/rom_syms.ld').read_text()
            for raw, palette_name, entries in pairs:
                for name, entry in zip((raw, palette_name), entries):
                    for suffix, value in (('START', entry['rom_start']), ('END', entry['rom_end'])):
                        self.assertEqual(symbols.count(f'PROVIDE({name}_ROM_{suffix} = 0x{value:08X});'), 1)
                self.assertNotIn('"' + raw + '"', (self.build / 'pal/port_segtab.c').read_text())
            for language in ('en', 'de', 'fr', 'es'):
                self.assertIn(f'msg_pal_{language}_ROM_START', symbols)
            self.assertEqual(self.snapshot('us'), us)
            self.assertEqual(table_path.read_bytes(), raw_bytes)

    def assert_palette_rejected(self, mutate):
        self.add_synthetic_palette_pairs()
        self.assertEqual(self.cli('--region', 'pal').returncode, 0)
        self.assertEqual(self.cli('--region', 'us').returncode, 0)
        us = self.snapshot('us')
        mutate()
        before = self.snapshot('pal')
        for existing in (True, False):
            if not existing:
                for name in self.generated('pal'):
                    (self.build / 'pal' / name).unlink()
                before = self.snapshot('pal')
            result = self.cli('--region', 'pal')
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(self.snapshot('pal'), before)
            self.assertEqual(self.snapshot('us'), us)
            self.assertEqual(list((self.build / 'pal').glob('.gen-build-*')), [])

    def change_palette_table(self, change):
        path = self.build / 'pal/rom_segments.json'
        table = json.loads(path.read_text())
        change(table)
        path.write_text(json.dumps(table))

    def change_palette_ld(self, change):
        path = self.decomp / 'ver/pal/papermario.ld'
        path.write_text(change(path.read_text()))

    def test_duplicate_code_type_is_not_a_palette_pair(self):
        self.assert_palette_rejected(lambda: self.change_palette_table(lambda t: t[-2].update(type='code')))

    def test_palette_pair_must_be_adjacent_in_rom_table(self):
        self.assert_palette_rejected(lambda: self.change_palette_table(lambda t: t.insert(-2,
            dict(name='gap', type='bin', rom_start=t[-2]['rom_start'], rom_end=t[-2]['rom_start']))))

    def test_palette_range_must_match_bin_size(self):
        self.assert_palette_rejected(lambda: (self.decomp / 'ver/pal/build/assets/pal/synthetic/texture_0.pal.bin').write_bytes(b'bad'))

    def test_image_range_must_match_bin_size(self):
        self.assert_palette_rejected(lambda: (self.decomp / 'ver/pal/build/assets/pal/synthetic/texture_0.png.bin').write_bytes(b'bad'))

    def test_palette_range_must_have_known_end(self):
        self.assert_palette_rejected(lambda: self.change_palette_table(lambda t: t[-2].update(rom_end=None)))

    def test_palette_range_start_must_match_previous_end(self):
        self.assert_palette_rejected(lambda: self.change_palette_table(lambda t: t[-2].update(rom_start=t[-2]['rom_start'] + 1)))

    def test_missing_palette_linker_mapping_is_rejected(self):
        self.assert_palette_rejected(lambda: self.change_palette_ld(lambda t: t.replace('texture_0.pal.o', 'unrelated.pal.o')))

    def test_multiple_palette_linker_mappings_are_rejected(self):
        self.assert_palette_rejected(lambda: self.change_palette_ld(lambda t: t +
            self.synthetic_data_section('other_colors', 'ver/pal/build/assets/pal/synthetic/texture_0.pal.o')))

    def test_duplicate_image_linker_section_is_rejected(self):
        self.assert_palette_rejected(lambda: self.change_palette_ld(lambda t: t +
            self.synthetic_data_section('graphic_0', 'ver/pal/build/assets/pal/synthetic/texture_0.png.o')))

    def test_palette_identity_cannot_collide_with_existing_name(self):
        self.assert_palette_rejected(lambda: self.change_palette_ld(lambda t: t.replace('colors_0', 'map')))

    def test_duplicate_rom_symbol_definition_is_rejected(self):
        self.assert_palette_rejected(lambda: self.change_palette_ld(lambda t: t + '    colors_0_ROM_START = __romPos;\n'))

    def test_palette_lma_must_use_its_own_start_symbol(self):
        self.assert_palette_rejected(lambda: self.change_palette_ld(lambda t: t.replace(
            'AT(colors_0_ROM_START)', 'AT(graphic_0_ROM_START)')))

    def test_palette_lma_chain_must_be_uninterrupted(self):
        self.assert_palette_rejected(lambda: self.change_palette_ld(lambda t: t.replace(
            '    colors_0_ROM_START =', '    __romPos += 16;\n    colors_0_ROM_START =')))

    def test_palette_bin_rule_must_match_linker_object(self):
        def mutate():
            path = self.decomp / 'build.ninja'
            path.write_text(path.read_text().replace('pal.o: bin ver/pal/build/assets/pal/synthetic/texture_0.pal.bin',
                                                   'pal.o: bin ver/pal/build/assets/pal/synthetic/other.bin'))
        self.assert_palette_rejected(mutate)

    def test_palette_object_shared_with_mixed_section_is_rejected(self):
        self.assert_palette_rejected(lambda: self.change_palette_ld(lambda t: t +
            '    .extra :\n        ver/pal/build/assets/pal/synthetic/texture_0.pal.o(.data);\n'
            '        ver/pal/build/src/main.c.o(.data);\n'))

    def test_image_object_shared_with_another_section_is_rejected(self):
        self.assert_palette_rejected(lambda: self.change_palette_ld(lambda t: t +
            '    .extra :\n        ver/pal/build/assets/pal/synthetic/texture_0.png.o(.data);\n'))

    def test_duplicate_palette_object_build_rule_is_rejected(self):
        def mutate():
            path = self.decomp / 'build.ninja'
            path.write_text(path.read_text() +
                'build ver/pal/build/assets/pal/synthetic/texture_0.pal.o: bin '
                'ver/pal/build/assets/pal/synthetic/texture_0.pal.bin\n')
        self.assert_palette_rejected(mutate)

    def test_palette_binary_must_be_regular_file(self):
        def mutate():
            path = self.decomp / 'ver/pal/build/assets/pal/synthetic/texture_0.pal.bin'
            path.unlink()
            path.mkdir()
        self.assert_palette_rejected(mutate)

    def test_palette_binary_cannot_alias_us_input(self):
        def mutate():
            source = self.write('refs/papermario/ver/us/build/palette.bin', 'x' * 8)
            path = self.decomp / 'ver/pal/build/assets/pal/synthetic/texture_0.pal.bin'
            path.unlink()
            path.symlink_to(source)
        self.assert_palette_rejected(mutate)

    def assert_bin_edge_rejected(self, transform, extensions=('png', 'pal'), expected_error=None):
        for extension in extensions:
            with self.subTest(extension=extension):
                self.make_fixture()
                def mutate():
                    path = self.decomp / 'build.ninja'
                    original = path.read_text()
                    obj = f'ver/pal/build/assets/pal/synthetic/texture_0.{extension}.o'
                    source = obj[:-2] + '.bin'
                    old = f'build {obj}: bin {source}'
                    self.assertEqual(original.count(old), 1)
                    path.write_text(original.replace(old, transform(old, obj, source)))
                    # Exercise E2c itself, even if the general parser would also
                    # reject the malformed graph before reaching preflight.
                    proof_error = io.StringIO()
                    with patch.multiple(gen_build, ROOT=str(self.root), DECOMP=str(self.decomp)), \
                            redirect_stderr(proof_error):
                        segs = gen_build.parse_ld(self.decomp / 'ver/pal/papermario.ld', 'pal')
                        table = json.loads((self.build / 'pal/rom_segments.json').read_text())
                        with self.assertRaises((SystemExit, ValueError)) as stopped:
                            gen_build.pal_rom_identities(table, segs, path)
                    if expected_error is not None:
                        self.assertIn(expected_error, proof_error.getvalue() + str(stopped.exception))
                self.assert_palette_rejected(mutate)

    def test_bin_edge_rejects_extra_image_input(self):
        self.assert_bin_edge_rejected(lambda line, obj, src: line + ' ' + src, ('png',))

    def test_bin_edge_rejects_extra_palette_input(self):
        self.assert_bin_edge_rejected(lambda line, obj, src: line + ' ' + src, ('pal',))

    def test_bin_edge_rejects_multiple_additional_inputs(self):
        self.assert_bin_edge_rejected(lambda line, obj, src: line + ' ' + src + ' other.bin')

    def test_bin_edge_rejects_extra_input_on_continuation(self):
        self.assert_bin_edge_rejected(lambda line, obj, src: line + ' $\n    ' + src)

    def test_bin_edge_rejects_missing_input(self):
        self.assert_bin_edge_rejected(lambda line, obj, src: f'build {obj}: bin')

    def test_bin_edge_rejects_wrong_rule(self):
        self.assert_bin_edge_rejected(lambda line, obj, src: f'build {obj}: phony {src}')

    def test_bin_edge_rejects_implicit_and_order_only_inputs(self):
        for marker in ('|', '||', '|@'):
            with self.subTest(marker=marker):
                self.assert_bin_edge_rejected(lambda line, obj, src: line + f' {marker} {src}')

    def test_bin_edge_rejects_variable_and_escaped_inputs(self):
        for value in ('$source', '${source}', '$$source', 'escaped$ input.bin', 'escaped$:input.bin'):
            with self.subTest(value=value):
                self.assert_bin_edge_rejected(lambda line, obj, src: f'build {obj}: bin {value}')

    def test_bin_edge_rejects_duplicate_output_aliases(self):
        for prefix in ('./', '/work/refs/papermario/'):
            with self.subTest(prefix=prefix):
                self.assert_bin_edge_rejected(lambda line, obj, src:
                    line + f'\nbuild {prefix}{obj} : bin {src}')

    def test_bin_edge_rejects_normalized_absolute_root_aliases(self):
        for root in (gen_build.C_DECOMP, str(self.decomp)):
            forms = (root, f'{os.path.dirname(root)}/./{os.path.basename(root)}',
                     root + '/scratch/..',
                     f'{os.path.dirname(root)}/scratch/../{os.path.basename(root)}')
            for alias in forms:
                with self.subTest(alias=alias):
                    self.assert_bin_edge_rejected(lambda line, obj, src:
                        line + f'\nbuild {alias}/{obj}: bin {src}',
                        expected_error='CI8/palette identity not proven')

    def test_bin_edge_rejects_foreign_absolute_output_roots(self):
        for root in (gen_build.C_DECOMP + '-other', str(self.decomp) + '-other', '/foreign'):
            with self.subTest(root=root):
                self.assert_bin_edge_rejected(lambda line, obj, src:
                    line + f'\nbuild {root}/{obj}: bin {src}',
                    expected_error='foreign absolute Ninja output')

    def test_bin_edge_rejects_output_root_escape_even_after_return(self):
        for root in (gen_build.C_DECOMP, str(self.decomp)):
            with self.subTest(root=root):
                self.assert_bin_edge_rejected(lambda line, obj, src:
                    line + f'\nbuild {root}/../{os.path.basename(root)}/{obj}: bin {src}',
                    expected_error='Ninja output escapes its identity root')
        self.assert_bin_edge_rejected(lambda line, obj, src:
            line + f'\nbuild ../{obj}: bin {src}',
            expected_error='Ninja output escapes its identity root')

    def test_bin_edge_rejects_multiple_mixed_output_aliases(self):
        self.assert_bin_edge_rejected(lambda line, obj, src:
            line + f'\nbuild ./ver/pal/build/assets/pal/synthetic/{Path(obj).name}: bin {src}' +
            f'\nbuild /work/refs/./papermario/{obj}: bin {src}' +
            f'\nbuild {self.decomp}/scratch/../{obj}: bin {src}',
            expected_error='CI8/palette identity not proven')

    def test_identity_output_keys_share_component_bounded_roots(self):
        obj = 'ver/pal/build/assets/pal/synthetic/image.pal.o'
        with patch.object(gen_build, 'DECOMP', str(self.decomp)):
            aliases = [obj, './' + obj, 'ver/pal/unused/../build/assets/pal/synthetic/image.pal.o']
            for root in (gen_build.C_DECOMP, str(self.decomp)):
                aliases.extend((root + '/' + obj,
                    f'{os.path.dirname(root)}/./{os.path.basename(root)}/{obj}',
                    root + '/scratch/../' + obj,
                    f'{os.path.dirname(root)}/scratch/../{os.path.basename(root)}/{obj}',
                    '//' + root + '/' + obj))
            for alias in aliases:
                with self.subTest(alias=alias):
                    self.assertEqual(gen_build.identity_ninja_output_key(alias), obj)

    def test_identity_output_keys_reject_root_escapes_and_prefix_confusion(self):
        with patch.object(gen_build, 'DECOMP', str(self.decomp)):
            for root in (gen_build.C_DECOMP, str(self.decomp)):
                for alias in (root + '-other/file.o', root + '/../papermario/file.o',
                              root + '/../../outside/file.o', root):
                    with self.subTest(alias=alias), self.assertRaises(ValueError):
                        gen_build.identity_ninja_output_key(alias)
            for alias in ('../file.o', 'a/../../file.o', '/foreign/file.o'):
                with self.subTest(alias=alias), self.assertRaises(ValueError):
                    gen_build.identity_ninja_output_key(alias)

    def test_bin_edge_rejects_additional_outputs(self):
        for implicit in ('', '| '):
            with self.subTest(implicit=implicit):
                self.assert_bin_edge_rejected(lambda line, obj, src:
                    f'build {obj} {implicit}extra.o: bin {src}')

    def test_bin_edge_rejects_escaped_or_variable_output_definitions(self):
        for output in ('$output', '${output}', 'escaped$ output.o', 'escaped$:output.o'):
            with self.subTest(output=output):
                self.assert_bin_edge_rejected(lambda line, obj, src:
                    line + f'\nbuild {output}: bin {src}')

    def test_bin_edge_rejects_unsupported_edge_bindings(self):
        for binding in ('dyndep = other.dd', 'command = other', 'version = us',
                        'version = $region', 'version = pal\n  version = pal'):
            with self.subTest(binding=binding):
                self.assert_bin_edge_rejected(lambda line, obj, src: line + '\n  ' + binding)

    def test_bin_edge_rejects_mid_token_continuation(self):
        self.assert_bin_edge_rejected(lambda line, obj, src:
            f'build {obj}: bin {src[:-4]}$\n    .bin')

    def test_bin_edge_rejects_unfinished_continuation(self):
        self.assert_bin_edge_rejected(lambda line, obj, src: line + ' $')

    def test_bin_edge_rejects_escaped_dollar_before_next_definition(self):
        self.assert_bin_edge_rejected(lambda line, obj, src: line + '$$\n' + line)

    def test_bin_edge_rejects_external_ninja_definitions(self):
        for directive in ('include', 'subninja'):
            with self.subTest(directive=directive):
                self.assert_bin_edge_rejected(lambda line, obj, src: line + f'\n{directive} other.ninja')

    def test_bin_edge_accepts_token_boundary_continuations_and_pal_binding(self):
        self.add_synthetic_palette_pairs()
        self.assertEqual(self.cli('--region', 'pal').returncode, 0)
        before = self.snapshot('pal')
        path = self.decomp / 'build.ninja'
        text = path.read_text()
        for extension in ('png', 'pal'):
            obj = f'ver/pal/build/assets/pal/synthetic/texture_0.{extension}.o'
            src = obj[:-2] + '.bin'
            text = text.replace(f'build {obj}: bin {src}',
                                f'build {obj}: $\n    bin $\n    {src}\n  version = pal')
        path.write_text(text)
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.snapshot('pal'), before)

    def test_global_follow_validation_on_all_platforms(self):
        for region in ('us', 'pal'):
            path = self.decomp / f'ver/{region}/papermario.ld'
            original = path.read_text()
            obj = f'ver/{region}/build/assets/{region}/font.c.o'
            base = f'    .font 0x00000000 :\n    {{\n        {obj}(.text);\n    }}\n'
            bss = f'    .font_bss (NOLOAD) : SUBALIGN(8)\n    {{\n        {obj}(.bss);\n    }}\n'
            valid = original.replace(f'    .font 0x00000000 :\n        {obj}(.text);',
                                     base + bss + '    . = ALIGN(., 16);\n    font_VRAM_END = .;')
            valid = valid.replace('.map 0x80240000 :', '.map font_VRAM_END :')
            self.assertNotEqual(valid, original)
            variants = {'late duplicate owner': valid + base,
                        'late BSS': valid.replace(bss, '') + bss,
                        'duplicate BSS': valid + bss}
            for platform in ('pc', 'handheld', 'miyoo'):
                with self.subTest(region=region, platform=platform):
                    path.write_text(original)
                    result = self.cli(platform, '--region', region)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    before = self.generated(region)
                    path.write_text(valid)
                    result = self.cli(platform, '--region', region)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    after = self.generated(region)
                    self.assertEqual(self.code_ranges(after['port_segtab.c']), self.code_ranges(before['port_segtab.c']))
                    self.assertEqual(after['rom_syms.ld'], before['rom_syms.ld'].replace(
                        b'PROVIDE(map_VRAM = 0x80240000);\n', b''))
                    self.assertEqual(after[platform + '.ninja'], before[platform + '.ninja'])
                    segments = json.loads(after['segments.json'])
                    self.assertEqual([x['name'] for x in segments],
                                     ['main', 'font', 'map', 'boot', 'messages'])
                    self.assertEqual(segments[2]['objs'], [f'ver/{region}/build/src/world/map.c.o'])
                    for label, invalid in variants.items():
                        for existing in (True, False):
                            with self.subTest(case=label, existing=existing):
                                if existing:
                                    path.write_text(valid)
                                    self.assertEqual(self.cli(platform, '--region', region).returncode, 0)
                                else:
                                    # Only synthetic output files owned by this test.
                                    destination = self.build / 'pal' if region == 'pal' else self.build
                                    for filename in self.generated(region):
                                        (destination / filename).unlink()
                                snapshot = self.snapshot(region)
                                other = self.snapshot('us' if region == 'pal' else 'pal')
                                path.write_text(invalid)
                                result = self.cli(platform, '--region', region)
                                self.assertNotEqual(result.returncode, 0)
                                self.assertIn('unproven predecessor VRAM end', result.stderr)
                                self.assertEqual(self.snapshot(region), snapshot)
                                self.assertEqual(self.snapshot('us' if region == 'pal' else 'pal'), other)
                                if not existing:
                                    self.assertEqual(self.generated(region), {})
            path.write_text(original)

    def test_unsupported_symbolic_linker_address_rejected(self):
        path = self.decomp / 'ver/pal/papermario.ld'
        path.write_text(path.read_text().replace('.main 0x80025C00 :', '.main UNRESOLVED_VRAM :'))
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('unsupported linker section', result.stderr)
        self.assertEqual(self.generated('pal'), {})

    def test_ninja_continuations(self):
        path = self.decomp / 'build.ninja'
        path.write_text(path.read_text().replace(': cc src/main.c', ': cc $\n  src/main.c'))
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_effect_render_failure_does_not_publish_partial_build(self):
        self.assertEqual(self.cli('--region', 'pal').returncode, 0)
        before = self.snapshot('pal')
        (self.decomp / 'assets/pal/effects/effect_defs.h').write_text(
            '    EFFECT_MISSING = 0x03,\nEFFECT_DEF_MISSING(fx_bad);\n')
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('missing effect macro', result.stderr)
        self.assertEqual(self.snapshot('pal'), before)
        self.assertEqual(list((self.build / 'pal').glob('.gen-build-*')), [])

    def test_existing_build_stamp_is_preserved(self):
        path = self.write('build/game/pal/pc/build_stamp.c', '/* keep this stamp */\n')
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(path.read_text(), '/* keep this stamp */\n')

    def test_pal_directory_alias_to_us_is_rejected(self):
        pal_table = (self.build / 'pal/rom_segments.json').read_bytes()
        (self.build / 'pal/rom_segments.json').unlink()
        (self.build / 'pal').rmdir()
        # Keep inputs valid so this specifically exercises the path guard.
        (self.build / 'rom_segments.json').write_bytes(pal_table)
        before = self.snapshot()
        (self.build / 'pal').symlink_to(self.build, target_is_directory=True)
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('PAL output directory must not be a symlink', result.stderr)
        self.assertEqual(self.snapshot(), before)

    def test_pal_file_alias_to_us_is_rejected(self):
        target = self.write('build/game/port_effects.c', 'keep US\n')
        (self.build / 'pal/port_effects.c').symlink_to(target)
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('PAL output escapes', result.stderr)
        self.assertEqual(target.read_text(), 'keep US\n')

    def test_pal_object_alias_to_us_is_rejected(self):
        target = self.write('build/game/pc/port/port_host.o', 'synthetic existing object')
        link = self.build / 'pal/pc/port/port_host.o'
        link.parent.mkdir(parents=True)
        link.symlink_to(target)
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('PAL output escapes', result.stderr)
        self.assertEqual(target.read_text(), 'synthetic existing object')

    def test_segment_generator_integration_with_synthetic_splat(self):
        segment_script = Path(__file__).with_name('rom_segments.py')
        shutil.copyfile(segment_script, self.script.with_name('rom_segments.py'))
        for region in ('us', 'pal'):
            table_path = self.build / ('pal/rom_segments.json' if region == 'pal' else 'rom_segments.json')
            table = json.loads(table_path.read_text())
            cfg = {'segments': [{'start': r['rom_start'], 'name': r['name'], 'type': r['type']} for r in table]}
            self.write(f'refs/papermario/ver/{region}/splat.yaml', json.dumps(cfg))
            table_path.unlink()
            result = subprocess.run([sys.executable, '-B', str(self.script.with_name('rom_segments.py')),
                                     '--region', region], cwd=self.root, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = self.cli('--region', region)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_image_include_prerequisites_are_checked_without_reading_binary(self):
        ninja = self.decomp / 'build.ninja'
        filename = 'ver/pal/build/img/synthetic.png.bin'
        ninja.write_text(ninja.read_text().replace('build inc_img_bins_pal: phony',
                                                  'build inc_img_bins_pal: phony ' + filename))
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('missing ' + filename, result.stderr)
        path = self.decomp / filename
        path.parent.mkdir(parents=True)
        path.write_bytes(b'\xff\xfe synthetic data, no image or ROM')
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_malformed_segment_table_does_not_publish(self):
        target = self.build / 'pal/rom_segments.json'
        for text in ('broken JSON', '{}', '[]', '[null]', '[{"name": true}]'):
            with self.subTest(text=text):
                target.write_text(text)
                result = self.cli('--region', 'pal')
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn('Traceback', result.stderr)
                self.assertEqual(self.generated('pal'), {})

    def test_foreign_linker_object_is_not_silently_skipped(self):
        path = self.decomp / 'ver/pal/papermario.ld'
        path.write_text(path.read_text().replace('ver/pal/build/src/main.c.o', 'ver/us/build/src/main.c.o'))
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('wrong-region linker object', result.stderr)
        self.assertEqual(self.generated('pal'), {})

    def test_empty_effect_headers_do_not_publish(self):
        (self.decomp / 'assets/pal/effects/effect_defs.h').write_text('/* empty */')
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('no fx_ entry points', result.stderr)
        self.assertEqual(self.generated('pal'), {})

    def test_missing_native_kit_source_does_not_publish(self):
        (self.root / 'kit/platform/miyoo_panel.c').unlink()
        result = self.cli('miyoo', '--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('missing native port source', result.stderr)
        self.assertEqual(self.generated('pal'), {})

    def test_pal_ninja_log_alias_to_us_is_rejected(self):
        target = self.write('build/game/.ninja_log', 'keep US log')
        (self.build / 'pal/.ninja_log').symlink_to(target)
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('PAL output escapes', result.stderr)
        self.assertEqual(target.read_text(), 'keep US log')

    def test_zero_length_rom_range_is_preserved(self):
        target = self.build / 'pal/rom_segments.json'
        table = json.loads(target.read_text())
        table.insert(0, {'name': 'heaps', 'type': 'code', 'rom_start': 0, 'rom_end': 0})
        target.write_text(json.dumps(table))
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('heaps_ROM_START = 0x00000000', (self.build / 'pal/rom_syms.ld').read_text())


    def test_us_generated_file_symlink_and_permissions_are_preserved(self):
        target = self.write('elsewhere/segments.json', 'old synthetic metadata')
        target.chmod(0o600)
        (self.build / 'segments.json').symlink_to(target)
        result = self.cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.build / 'segments.json').is_symlink())
        self.assertEqual(target.stat().st_mode & 0o777, 0o600)
        self.assertTrue(json.loads(target.read_text()))

    def test_wrong_region_source_path_rejected(self):
        path = self.decomp / 'build.ninja'
        path.write_text(path.read_text().replace(': cc assets/pal/font.c', ': cc assets/us/font.c'))
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('wrong region', result.stderr)

    def test_inconsistent_segment_end_rejected(self):
        path = self.build / 'pal/rom_segments.json'
        table = json.loads(path.read_text())
        table[0]['rom_end'] = 15
        path.write_text(json.dumps(table))
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('inconsistent ROM segment end', result.stderr)

    def test_common_bss_object_is_not_lost_or_duplicated(self):
        path = self.decomp / 'ver/pal/papermario.ld'
        path.write_text(path.read_text().replace('(.bss);', '(COMMON);'))
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)
        segments = json.loads((self.build / 'pal/segments.json').read_text())
        main = next(segment for segment in segments if segment['name'] == 'main')
        self.assertEqual(main['objs'], ['ver/pal/build/src/main.c.o'])

    def test_asset_wrapper_helper_selects_pal_and_uses_separate_output(self):
        self.write('refs/papermario/assets/pal/synthetic.inc.c', '/* synthetic include */')
        with patch.multiple(gen_build, ROOT=str(self.root), DECOMP=str(self.decomp)):
            self.assertEqual(gen_build.write_asset_wrappers('pal'), 1)
        wrapper = self.build / 'pal/assetwrap/synthetic.inc.c'
        self.assertIn('#include_next "synthetic.inc.c"', wrapper.read_text())
        self.assertFalse((self.build / 'assetwrap').exists())

    def test_existing_sanitizer_and_toolchain_options_remain_supported(self):
        result = self.cli(extra_env={'PM64_SAN': '-fsanitize=address'})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('-fsanitize=address', (self.build / 'pc_game.mk').read_text())
        result = self.cli('miyoo', '--region', 'pal', extra_env={'MIYOO_TOOLCHAIN': '/synthetic-toolchain'})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('/synthetic-toolchain/usr/bin/arm-linux-gnueabihf-gcc',
                      (self.build / 'pal/miyoo.ninja').read_text())


    def prepare_published_outputs(self):
        self.assertEqual(self.cli().returncode, 0)
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)

    def assert_collision_preserves_outputs(self, expected):
        before_pal, before_us = self.snapshot('pal'), self.snapshot()
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(expected, result.stderr)
        self.assertNotIn('Traceback', result.stderr)
        self.assertEqual(self.snapshot('pal'), before_pal)
        self.assertEqual(self.snapshot(), before_us)
        self.assertEqual(list((self.build / 'pal').glob('.gen-build-*')), [])

    def test_output_symlink_to_segment_input_is_rejected_before_publication(self):
        self.prepare_published_outputs()
        output = self.build / 'pal/port_effects.c'
        output.unlink()
        output.symlink_to('rom_segments.json')
        self.assert_collision_preserves_outputs('output/input collision')
        self.assertEqual(output.readlink(), Path('rom_segments.json'))

    def test_output_symlink_to_other_generated_file_is_rejected(self):
        self.prepare_published_outputs()
        output = self.build / 'pal/port_effects.c'
        output.unlink()
        output.symlink_to('rom_syms.ld')
        self.assert_collision_preserves_outputs('output/output collision')
        self.assertEqual(output.readlink(), Path('rom_syms.ld'))

    def test_distinct_relative_symlink_chains_to_same_output_are_rejected(self):
        self.prepare_published_outputs()
        self.write('build/game/pal/shared-target', 'keep synthetic shared target')
        links = self.build / 'pal/links'
        links.mkdir()
        (links / 'first').symlink_to('../shared-target')
        (links / 'second').symlink_to('first')
        for name, destination in (('port_effects.c', 'links/first'), ('rom_syms.ld', 'links/second')):
            path = self.build / 'pal' / name
            path.unlink()
            path.symlink_to(destination)
        self.assert_collision_preserves_outputs('output/output collision')
        self.assertTrue((links / 'first').is_symlink())
        self.assertTrue((links / 'second').is_symlink())

    def test_relative_symlink_chain_to_input_is_rejected(self):
        self.prepare_published_outputs()
        links = self.build / 'pal/links'
        links.mkdir()
        (links / 'input').symlink_to('../rom_segments.json')
        output = self.build / 'pal/port_effects.c'
        output.unlink()
        output.symlink_to('links/input')
        self.assert_collision_preserves_outputs('output/input collision')

    def test_output_hardlink_to_input_is_rejected(self):
        self.prepare_published_outputs()
        output = self.build / 'pal/port_effects.c'
        output.unlink()
        output.hardlink_to(self.build / 'pal/rom_segments.json')
        self.assert_collision_preserves_outputs('output/input hardlink collision')

    def test_output_hardlink_to_other_output_is_rejected(self):
        self.prepare_published_outputs()
        output = self.build / 'pal/port_effects.c'
        output.unlink()
        output.hardlink_to(self.build / 'pal/rom_syms.ld')
        self.assert_collision_preserves_outputs('output/output hardlink collision')

    def test_future_pal_object_hardlink_to_us_object_is_rejected(self):
        self.prepare_published_outputs()
        us_object = self.write('build/game/pc/src/main.c.o', 'keep synthetic US object')
        output = self.build / 'pal/pc/src/main.c.o'
        output.parent.mkdir(parents=True)
        output.hardlink_to(us_object)
        self.assert_collision_preserves_outputs('output/input hardlink collision')

    def test_future_pal_log_hardlink_to_us_log_is_rejected(self):
        self.prepare_published_outputs()
        us_log = self.write('.ninja_log', 'keep synthetic US log')
        (self.build / 'pal/.ninja_log').hardlink_to(us_log)
        self.assert_collision_preserves_outputs('output/input hardlink collision')
        self.assertEqual(us_log.read_text(), 'keep synthetic US log')

    def test_unrelated_symlink_target_inside_pal_remains_supported(self):
        self.prepare_published_outputs()
        output = self.build / 'pal/port_effects.c'
        output.unlink()
        output.symlink_to('private-effects.c')
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(output.is_symlink())
        self.assertIn('load_effect(0x02)', output.read_text())

    def place_required_segment_last_without_end(self, name):
        path = self.build / 'pal/rom_segments.json'
        table = json.loads(path.read_text())
        wanted = next(entry for entry in table if entry['name'] == name)
        table = [entry for entry in table if entry['name'] is not None and entry['name'] != name] + [wanted]
        for index, entry in enumerate(table):
            entry['rom_start'] = index * 16
            entry['rom_end'] = (index + 1) * 16 if index + 1 < len(table) else None
        path.write_text(json.dumps(table))

    def assert_required_range_failure(self, name):
        before_pal, before_us = self.snapshot('pal'), self.snapshot()
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('required PAL ROM range: ' + name, result.stderr)
        self.assertEqual(self.snapshot('pal'), before_pal)
        self.assertEqual(self.snapshot(), before_us)

    def test_missing_end_of_spanish_messages_is_rejected(self):
        self.prepare_published_outputs()
        self.place_required_segment_last_without_end('msg_pal_es')
        self.assert_required_range_failure('msg_pal_es')

    def test_missing_end_of_german_messages_is_rejected(self):
        self.prepare_published_outputs()
        self.place_required_segment_last_without_end('msg_pal_de')
        self.assert_required_range_failure('msg_pal_de')

    def test_missing_end_of_native_segment_is_rejected(self):
        self.prepare_published_outputs()
        self.place_required_segment_last_without_end('main')
        self.assert_required_range_failure('main')

    def test_empty_required_language_and_native_ranges_are_rejected(self):
        self.prepare_published_outputs()
        path = self.build / 'pal/rom_segments.json'
        original = path.read_text()
        for name in ('msg_pal_de', 'msg_pal_es', 'main'):
            with self.subTest(segment=name):
                table = json.loads(original)
                index = next(i for i, entry in enumerate(table) if entry['name'] == name)
                table[index]['rom_end'] = table[index]['rom_start']
                table[index + 1]['rom_start'] = table[index]['rom_start']
                path.write_text(json.dumps(table))
                self.assert_required_range_failure(name)

    def test_required_range_outside_32bit_rom_space_is_rejected(self):
        self.prepare_published_outputs()
        path = self.build / 'pal/rom_segments.json'
        table = json.loads(path.read_text())
        table[-2]['rom_end'] = 0x100000000
        table[-1]['rom_start'] = 0x100000000
        path.write_text(json.dumps(table))
        self.assert_required_range_failure('msg_pal_es')

    def test_pinned_native_bss_segment_can_have_zero_rom_length(self):
        ld = self.decomp / 'ver/pal/papermario.ld'
        ninja = self.decomp / 'build.ninja'
        path = self.build / 'pal/rom_segments.json'
        original_ld, original_ninja, original_table = ld.read_text(), ninja.read_text(), path.read_text()
        for name in ('heaps', 'heaps2', 'heaps3', 'heaps4', 'texture_memory', 'model_clear_render_tasks'):
            with self.subTest(segment=name):
                self.write(f'refs/papermario/src/{name}.c', '/* synthetic BSS-only source at pinned path */')
                ld.write_text(f'    .{name} 0x80195000 :\n        ver/pal/build/src/{name}.c.o(.bss);\n' + original_ld)
                ninja.write_text(original_ninja + f'\nbuild ver/pal/build/src/{name}.c.o: cc src/{name}.c\n  cppflags = -DVERSION_PAL\n')
                table = json.loads(original_table)
                table.insert(0, {'name': name, 'type': 'code', 'rom_start': 0, 'rom_end': 0})
                path.write_text(json.dumps(table))
                result = self.cli('--region', 'pal')
                self.assertEqual(result.returncode, 0, result.stderr)
                symbols = (self.build / 'pal/rom_syms.ld').read_text()
                self.assertIn(name + '_ROM_START = 0x00000000', symbols)
                self.assertIn(name + '_ROM_END = 0x00000000', symbols)
                self.assertNotIn('"' + name + '"', (self.build / 'pal/port_segtab.c').read_text())

    def test_bss_segment_name_alone_does_not_allow_empty_native_code(self):
        ld = self.decomp / 'ver/pal/papermario.ld'
        ld.write_text(ld.read_text().replace('.main ', '.heaps ').replace('.main_bss ', '.heaps_bss '))
        path = self.build / 'pal/rom_segments.json'
        table = json.loads(path.read_text())
        table[0].update(name='heaps', rom_end=0)
        table[1]['rom_start'] = 0
        path.write_text(json.dumps(table))
        self.assert_required_range_failure('heaps')

    def test_bss_type_tag_does_not_allow_empty_native_code(self):
        path = self.build / 'pal/rom_segments.json'
        table = json.loads(path.read_text())
        table[0].update(type='bss', rom_end=0)
        table[1]['rom_start'] = 0
        path.write_text(json.dumps(table))
        self.assert_required_range_failure('main')

    def test_duplicate_required_segment_name_is_rejected(self):
        path = self.build / 'pal/rom_segments.json'
        table = json.loads(path.read_text())
        table[-2]['name'] = 'msg_pal_de'
        # Keep all required names present while adding a second German segment.
        table[-1] = {'name': 'msg_pal_es', 'type': 'pm_msg', 'rom_start': table[-2]['rom_end'],
                     'rom_end': table[-2]['rom_end'] + 16}
        table.append({'name': None, 'type': None, 'rom_start': table[-1]['rom_end'], 'rom_end': None})
        path.write_text(json.dumps(table))
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('ambiguous PAL ROM segment name', result.stderr)

    def test_complete_pal_ranges_generate_all_required_start_end_symbols(self):
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)
        symbols = (self.build / 'pal/rom_syms.ld').read_text()
        for name in ('main', 'font', 'map', 'msg_pal_en', 'msg_pal_de', 'msg_pal_fr', 'msg_pal_es'):
            self.assertIn('PROVIDE(' + name + '_ROM_START =', symbols)
            self.assertIn('PROVIDE(' + name + '_ROM_END =', symbols)

    def test_failure_on_second_replace_reports_error_and_preserves_us(self):
        self.prepare_published_outputs()
        us = self.snapshot()
        first = self.build / 'pal/assets_noload.ld'
        second = self.build / 'pal/assets_progbits.ld'
        first_inode, second_inode = first.stat().st_ino, second.stat().st_ino
        original_replace = os.replace
        calls = []

        def fail_second(source, target):
            calls.append(target)
            if len(calls) == 2:
                raise OSError('synthetic second replace failure')
            return original_replace(source, target)

        stdout, stderr = io.StringIO(), io.StringIO()
        with patch.multiple(gen_build, ROOT=str(self.root), DECOMP=str(self.decomp)), \
                patch.object(gen_build.os, 'replace', side_effect=fail_second), \
                redirect_stdout(stdout), redirect_stderr(stderr):
            with self.assertRaises(SystemExit) as stopped:
                gen_build.main(['--region', 'pal'])
        self.assertEqual(stopped.exception.code, 1)
        self.assertIn('synthetic second replace failure', stderr.getvalue())
        self.assertNotIn('gen_build.py: wrote', stdout.getvalue())
        self.assertEqual(len(calls), 2)
        self.assertNotEqual(first.stat().st_ino, first_inode)
        self.assertEqual(second.stat().st_ino, second_inode)
        self.assertEqual(self.snapshot(), us)
        self.assertEqual(list((self.build / 'pal').glob('.gen-build-*')), [])

    def test_publication_chmod_failure_reports_error_without_replacing_outputs(self):
        self.prepare_published_outputs()
        pal, us = self.snapshot('pal'), self.snapshot()
        stdout, stderr = io.StringIO(), io.StringIO()
        with patch.multiple(gen_build, ROOT=str(self.root), DECOMP=str(self.decomp)), \
                patch.object(gen_build.os, 'chmod', side_effect=PermissionError('synthetic permission failure')), \
                redirect_stdout(stdout), redirect_stderr(stderr):
            with self.assertRaises(SystemExit) as stopped:
                gen_build.main(['--region', 'pal'])
        self.assertEqual(stopped.exception.code, 1)
        self.assertIn('synthetic permission failure', stderr.getvalue())
        self.assertNotIn('gen_build.py: wrote', stdout.getvalue())
        self.assertEqual(self.snapshot('pal'), pal)
        self.assertEqual(self.snapshot(), us)
        self.assertEqual(list((self.build / 'pal').glob('.gen-build-*')), [])



    def test_pal_numeric_rom_name_is_link_safe_and_identity_unchanged(self):
        table = self.build / 'pal/rom_segments.json'
        ranges = json.loads(table.read_text())
        marker = ranges[-1]['rom_start']
        ranges.insert(-1, {'name': '1942C40', 'type': 'code',
                           'rom_start': marker, 'rom_end': marker + 16})
        ranges[-1]['rom_start'] = marker + 16
        table.write_text(json.dumps(ranges))
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)
        symbols = (self.build / 'pal/rom_syms.ld').read_text()
        self.assertIn('PROVIDE(seg_1942C40_ROM_START = ', symbols)
        self.assertNotIn('PROVIDE(1942C40_ROM_START', symbols)
        self.assertIn('1942C40', [x['name'] for x in json.loads(table.read_text())])

    def test_pal_numeric_link_name_collision_fails_closed(self):
        table = self.build / 'pal/rom_segments.json'
        ranges = json.loads(table.read_text())
        marker = ranges[-1]['rom_start']
        for i, name in enumerate(('1942C40', 'seg_1942C40')):
            ranges.insert(-1, {'name': name, 'type': 'code',
                               'rom_start': marker + i*16, 'rom_end': marker + (i+1)*16})
        ranges[-1]['rom_start'] = marker + 32
        table.write_text(json.dumps(ranges))
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('linker symbol collision', result.stderr)

    def test_pal_zero_vram_data_size_requires_declared_data_segment(self):
        header = self.decomp / 'ver/pal/build/include/ld_addrs.h'
        header.write_text('extern Addr font_DATA_SIZE;\n')
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)
        symbols = (self.build / 'pal/rom_syms.ld').read_text()
        self.assertIn('PROVIDE(font_DATA_SIZE = 0x00000010);', symbols)
        splat = self.decomp / 'ver/pal/splat.yaml'
        splat.write_text(splat.read_text().replace('      - type: .data', '      - type: .rodata'))
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('PROVIDE(font_DATA_SIZE', (self.build / 'pal/rom_syms.ld').read_text())

    def test_pal_follow_uses_verified_explicit_splat_vram(self):
        ld = self.decomp / 'ver/pal/papermario.ld'
        obj = 'ver/pal/build/assets/pal/font.c.o'
        text = ld.read_text().replace(
            f'    .font 0x00000000 :\n        {obj}(.text);',
            f'    .font 0x00000000 : AT(font_ROM_START) SUBALIGN(8)\n'
            f'    {{\n        {obj}(.text);\n    }}\n'
            '    . = ALIGN(., 16);\n    font_VRAM_END = .;')
        ld.write_text(text.replace('.map 0x80240000 :', '.map font_VRAM_END :'))
        header = self.decomp / 'ver/pal/build/include/ld_addrs.h'
        header.write_text('extern Addr map_VRAM;\n')
        splat = self.decomp / 'ver/pal/splat.yaml'
        splat.write_text(splat.read_text().replace(
            '    vram: 0x80240000\n', '    vram: 0x10\n    follows_vram: font\n'))
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)
        symbols = (self.build / 'pal/rom_syms.ld').read_text()
        self.assertIn('PROVIDE(map_VRAM = 0x00000010);', symbols)
        splat.write_text(splat.read_text().replace('follows_vram: font', 'follows_vram: main'))
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('PROVIDE(map_VRAM', (self.build / 'pal/rom_syms.ld').read_text())

    def test_pal_missing_splat_is_rejected_before_publication(self):
        (self.decomp / 'ver/pal/splat.yaml').unlink()
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('splat.yaml', result.stderr)
        self.assertFalse((self.build / 'pal/rom_syms.ld').exists())

    def test_pal_splat_duplicate_code_segment_is_rejected(self):
        splat = self.decomp / 'ver/pal/splat.yaml'
        splat.write_text(splat.read_text() +
                         '  - name: font\n    type: code\n    start: 0x10\n    vram: 0\n')
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('ambiguous PAL Splat code segment', result.stderr)

    def test_pal_splat_nested_name_does_not_override_parent(self):
        splat = self.decomp / 'ver/pal/splat.yaml'
        splat.write_text(splat.read_text().replace(
            '      - type: .data\n        name: font',
            '      - type: .data\n        name: nested_data'))
        facts = gen_build.pal_splat_code_facts(splat)
        self.assertIn('font', facts)
        self.assertTrue(facts['font']['has_data'])
        self.assertNotIn('nested_data', facts)

    def test_pal_splat_nested_mapping_data_is_detected_without_clobbering_parent(self):
        # Mirrors real PAL: `- start: ...` and a nested `type: .data`.
        splat = self.decomp / 'ver/pal/splat.yaml'
        original = splat.read_text()
        needle = '      - type: .data\n        name: font'
        self.assertIn(needle, original)
        splat.write_text(original.replace(
            needle,
            '      - start: 0x10\n        type: .data\n        name: child_data'))
        facts = gen_build.pal_splat_code_facts(splat)
        self.assertIn('font', facts)
        self.assertTrue(facts['font']['has_data'])
        self.assertEqual(facts['font']['start'], 0x10)
        self.assertEqual(facts['font']['vram'], 0)
        self.assertNotIn('child_data', facts)
        header = self.decomp / 'ver/pal/build/include/ld_addrs.h'
        header.write_text('extern Addr font_DATA_SIZE;\n')
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('PROVIDE(font_DATA_SIZE = 0x00000010);',
                      (self.build / 'pal/rom_syms.ld').read_text())

    def test_pal_json_splat_reads_only_top_level_code_facts(self):
        # rom_segments integration historically writes JSON (valid YAML).
        splat = self.decomp / 'ver/pal/splat.yaml'
        splat.write_text(json.dumps({'segments': [
            {'name': 'font', 'type': 'code', 'start': 16, 'vram': 0,
             'subsegments': [{'start': 16, 'type': '.data', 'name': 'nested'}]},
            {'name': 'map', 'type': 'code', 'start': 32, 'vram': 0x80240000},
            {'name': None, 'type': None, 'start': 48},
        ]}))
        facts = gen_build.pal_splat_code_facts(splat)
        self.assertEqual(facts['font']['start'], 16)
        self.assertEqual(facts['font']['vram'], 0)
        self.assertTrue(facts['font']['has_data'])
        self.assertFalse(facts['map']['has_data'])
        self.assertNotIn('nested', facts)
        self.assertEqual(set(facts), {'font', 'map'})
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_pal_json_splat_rejects_ambiguous_and_malformed_segments(self):
        splat = self.decomp / 'ver/pal/splat.yaml'
        cases = (
            ('{"segments": {}}', 'top-level segments list'),
            ('{"segments": [null]}', 'invalid PAL Splat JSON segment'),
            (json.dumps({'segments': [
                {'name': 'font', 'type': 'code'},
                {'name': 'font', 'type': 'code'}]}), 'ambiguous PAL Splat code segment'),
            ('{"segments": [', 'invalid PAL Splat JSON'),
        )
        for body, expected in cases:
            with self.subTest(body=body):
                splat.write_text(body)
                result = self.cli('--region', 'pal')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)

    def test_pal_pause_gfx_overlays_follow_data_backed_metadata(self):
        splat = self.decomp / 'ver/pal/splat.yaml'
        for name in ('pause_gfx_en', 'pause_gfx_de', 'pause_gfx_fr', 'pause_gfx_es'):
            splat.write_text(splat.read_text() +
                f'  - type: code\n    name: {name}\n    start: 0x20\n'
                '    vram: 0x8027AA10\n    subsegments:\n'
                f'      - start: 0x20\n        type: .data\n        name: {name}\n')
        ld = self.decomp / 'ver/pal/papermario.ld'
        ld.write_text(ld.read_text() + ''.join(
            f'    .{name} 0x8027AA10 :\n'
            for name in ('pause_gfx_en', 'pause_gfx_de', 'pause_gfx_fr', 'pause_gfx_es')))
        table = self.build / 'pal/rom_segments.json'
        ranges = json.loads(table.read_text())
        marker = ranges[-1]['rom_start']
        for i, name in enumerate(('pause_gfx_en', 'pause_gfx_de', 'pause_gfx_fr', 'pause_gfx_es')):
            ranges.insert(-1, {'name': name, 'type': 'code', 'rom_start': marker + 16*i,
                               'rom_end': marker + 16*(i+1)})
            # Mirror new range starts in top-level Splat metadata.
            splat.write_text(splat.read_text().replace(
                f'name: {name}\n    start: 0x20',
                f'name: {name}\n    start: 0x{marker + 16*i:X}'))
        ranges[-1]['rom_start'] = marker + 64
        table.write_text(json.dumps(ranges))
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)
        symbols = (self.build / 'pal/rom_syms.ld').read_text()
        for name in ('pause_gfx_en', 'pause_gfx_de', 'pause_gfx_fr', 'pause_gfx_es'):
            for suffix in ('TEXT_START','TEXT_END','DATA_START','RODATA_END','BSS_START','BSS_END'):
                self.assertIn(f'PROVIDE({name}_{suffix} = 0);', symbols)

class InputPathNormalizationTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.decomp = self.root / 'decomp'
        self.decomp.mkdir()
        self.enterContext(patch.object(gen_build, 'DECOMP', str(self.decomp)))
        (self.decomp / 'src').mkdir()
        (self.decomp / 'src/input.c').write_text('/* synthetic */')

    def test_missing_components_and_multiple_parents(self):
        for path in ('src/absent/../input.c', 'src/a/b/../../input.c',
                     './src/./input.c', 'src/absent/.././input.c'):
            with self.subTest(path=path):
                self.assertEqual(gen_build.normalized_input(path), 'src/input.c')
                self.assertTrue(Path(gen_build.local_input(path)).is_file())

    def assert_metadata_failure(self, code):
        real_lstat = Path.lstat
        for region in ('us', 'pal'):
            fault = self.decomp / f'ver/{region}/build/blocked/link'
            fault.parent.mkdir(parents=True, exist_ok=True)
            (fault.parent.parent / 'input.h').write_text('/* synthetic */')
            failure = OSError(code, 'synthetic metadata failure', str(fault))

            def fail_lstat(path, *args, **kwargs):
                if path == fault:
                    raise failure
                return real_lstat(path, *args, **kwargs)

            with self.subTest(region=region, errno=code), patch.object(Path, 'lstat', fail_lstat):
                with self.assertRaisesRegex(ValueError, 'cannot inspect PMRET input component') as raised:
                    gen_build.normalized_input(f'ver/{region}/build/blocked/link/../../input.h')
            self.assertIn(str(fault), str(raised.exception))
            self.assertIs(raised.exception.__cause__, failure)

    def test_permission_metadata_errors(self):
        for code in (errno.EACCES, errno.EPERM):
            self.assert_metadata_failure(code)

    def test_io_metadata_error(self):
        self.assert_metadata_failure(errno.EIO)

    def test_other_metadata_errors(self):
        for code in (errno.ENOMEM, errno.ELOOP, errno.ENAMETOOLONG, None):
            self.assert_metadata_failure(code)

    def test_not_directory_metadata_error(self):
        self.assert_metadata_failure(errno.ENOTDIR)

    def test_existing_ordinary_directory_can_be_cancelled(self):
        for region in ('us', 'pal'):
            directory = self.decomp / f'ver/{region}/build/ordinary'
            directory.mkdir(parents=True)
            expected = directory.parent / 'input.h'
            expected.write_text('/* synthetic */')
            self.assertEqual(gen_build.local_input(f'ver/{region}/build/ordinary/../input.h'), str(expected))

    def test_known_dependency_shapes_with_synthetic_files(self):
        shapes = [('include/', 'area_mac/mac_01', 'toad_house/blanket.png.h'),
                  ('include/', 'area_kzn/kzn_19', 'lava_piranha/vine.png.h'),
                  ('', 'area_mac/mac_01', 'toad_house/blanket.png.bin'),
                  ('', 'area_kzn/kzn_19', 'lava_piranha/vine.png.bin'),
                  ('', 'area_kzn/kzn_19', 'lava_piranha/vine.pal.bin')]
        for region in ('us', 'pal'):
            for prefix, discarded, target in shapes:
                with self.subTest(region=region, target=target):
                    root = f'ver/{region}/build/{prefix}world/'
                    expected = self.decomp / (root + target)
                    expected.parent.mkdir(parents=True, exist_ok=True)
                    expected.write_bytes(b'synthetic')
                    self.assertFalse((self.decomp / (root + discarded)).exists())
                    alias = root + discarded + '/../../' + target
                    self.assertEqual(gen_build.local_input(alias), str(expected))
                    self.assertTrue(Path(gen_build.local_input(alias)).is_file())

    def test_known_absolute_roots(self):
        for root in (gen_build.C_DECOMP, str(self.decomp)):
            path = root + '/src/absent/../input.c'
            self.assertEqual(gen_build.normalized_input(path), 'src/input.c')

    def test_escape_and_foreign_absolute_paths(self):
        for path in ('../input.c', 'src/../../input.c', '/etc/passwd',
                     gen_build.C_DECOMP + '-foreign/src/input.c',
                     gen_build.C_DECOMP + '/../input.c'):
            with self.subTest(path=path):
                with self.assertRaises(ValueError):
                    gen_build.normalized_input(path)

    def test_internal_symlink_is_preserved_without_parent_cancellation(self):
        (self.decomp / 'alias').symlink_to('src', target_is_directory=True)
        self.assertEqual(gen_build.normalized_input('alias/./input.c'), 'alias/input.c')
        self.assertTrue(Path(gen_build.local_input('alias/input.c')).is_file())
        with self.assertRaisesRegex(ValueError, 'symlink'):
            gen_build.normalized_input('alias/../src/input.c')

    def test_relative_symlink_chain_cannot_escape(self):
        (self.decomp / 'first').symlink_to('second')
        (self.decomp / 'second').symlink_to('../outside')
        (self.root / 'outside').write_text('/* synthetic */')
        with self.assertRaisesRegex(ValueError, 'outside its root'):
            gen_build.normalized_input('first')
        with self.assertRaisesRegex(ValueError, 'symlink'):
            gen_build.normalized_input('first/../src/input.c')

    def test_dangling_symlink_cannot_be_cancelled(self):
        (self.decomp / 'dangling').symlink_to('absent')
        with self.assertRaisesRegex(ValueError, 'symlink'):
            gen_build.normalized_input('dangling/../src/input.c')

    def test_symlink_loop_is_reported(self):
        (self.decomp / 'loop').symlink_to('loop')
        with self.assertRaisesRegex(ValueError, 'symlink loop'):
            gen_build.normalized_input('loop')


class LinkerFollowParserTests(unittest.TestCase):
    """Author-created linker text only: no real split, ROM or asset inputs."""
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))

    def block(self, name, address, region, bss=False):
        obj = f'ver/{region}/build/src/{name}.c.o'
        text = (f'    .{name} {address} : AT({name}_ROM_START) SUBALIGN(8)\n'
                f'    {{\n        {obj}(.text);\n        . = ALIGN(., 16);\n    }}\n')
        if bss:
            text += (f'    {name}_bss_VRAM = ADDR(.{name}_bss);\n'
                     f'    .{name}_bss (NOLOAD) : SUBALIGN(8)\n'
                     f'    {{\n        {obj}(.bss);\n        . = ALIGN(., 16);\n    }}\n')
        return (text + f'    __romPos += SIZEOF(.{name});\n'
                '    __romPos = ALIGN(__romPos, 16);\n    . = ALIGN(., 16);\n'
                f'    {name}_ROM_END = __romPos;\n    {name}_VRAM_END = .;\n')

    def fixture(self, region, bss=False):
        return (self.block('alpha', '0x80200000', region, bss) +
                '    beta_ROM_START = __romPos;\n    beta_VRAM = ADDR(.beta);\n' +
                self.block('beta', 'alpha_VRAM_END', region, bss))

    def parse(self, text, region):
        path = self.root / 'synthetic.ld'
        path.write_text(text)
        return gen_build.parse_ld(path, region)

    def reject(self, transform):
        for region in ('us', 'pal'):
            with self.subTest(region=region):
                with self.assertRaisesRegex(ValueError, 'unsupported linker section address'):
                    self.parse(transform(self.fixture(region, True), region), region)

    def test_valid_follow(self):
        for region in ('us', 'pal'):
            for bss in (False, True):
                with self.subTest(region=region, bss=bss):
                    segs = self.parse(self.fixture(region, bss), region)
                    self.assertEqual([x['name'] for x in segs], ['alpha', 'beta'])
                    self.assertEqual(segs[0]['vram'], 0x80200000)
                    self.assertIsNone(segs[1]['vram'])
                    self.assertEqual(segs[1]['vram_follow'],
                                     {'segment': 'alpha', 'symbol': 'alpha_VRAM_END'})
                    self.assertEqual(segs[1]['objs'], [f'ver/{region}/build/src/beta.c.o'])
                    self.assertEqual(gen_build.seg_class(segs[1], region), 'follow')

    def test_follow_chain(self):
        for region in ('us', 'pal'):
            segs = self.parse(self.fixture(region, True) +
                              self.block('gamma', 'beta_VRAM_END', region, True), region)
            self.assertEqual([s['name'] for s in segs], ['alpha', 'beta', 'gamma'])
            self.assertEqual(segs[2]['vram_follow']['segment'], 'beta')
            self.assertEqual(segs[2]['objs'], [f'ver/{region}/build/src/gamma.c.o'])

    def test_unknown_symbol(self):
        self.reject(lambda t, r: t.replace('.beta alpha_VRAM_END :', '.beta UNKNOWN :'))

    def test_missing_definition(self):
        self.reject(lambda t, r: t.replace('    alpha_VRAM_END = .;\n', ''))

    def test_duplicate_definition(self):
        self.reject(lambda t, r: t + '    alpha_VRAM_END = .;\n')

    def test_duplicate_predecessor_section(self):
        self.reject(lambda t, r: '    .alpha 0x80100000 :\n    {\n    }\n' + t)

    def test_duplicate_predecessor_after_follower(self):
        self.reject(lambda t, r: t + '    .alpha 0x80300000 :\n    {\n    }\n')

    def test_bss_after_follower(self):
        self.reject(lambda t, r: self.fixture(r) +
                    '    .alpha_bss (NOLOAD) : SUBALIGN(8)\n    {\n    }\n')

    def test_duplicate_predecessor_bss(self):
        self.reject(lambda t, r: t +
                    '    .alpha_bss (NOLOAD) : SUBALIGN(8)\n    {\n    }\n')

    def test_duplicate_follower_after_relation(self):
        self.reject(lambda t, r: t + '    .beta 0x80300000 :\n    {\n    }\n')

    def test_predecessor_bss_with_conflicting_address(self):
        self.reject(lambda t, r: t.replace('.alpha_bss (NOLOAD)', '.alpha_bss 0x80400000'))

    def test_forward_end_definition(self):
        self.reject(lambda t, r: t.replace('    alpha_VRAM_END = .;\n', '') +
                    '    alpha_VRAM_END = .;\n')

    def test_symbolic_first_segment(self):
        self.reject(lambda t, r: t.replace('.alpha 0x80200000 :',
                                           '.alpha alpha_VRAM_END :'))

    def test_compound_redefinition(self):
        self.reject(lambda t, r: t + '    alpha_VRAM_END += 16;\n')

    def test_other_compound_redefinitions(self):
        for operator in ('%=', '<<=', '>>=', '^=', '-='):
            with self.subTest(operator=operator):
                self.reject(lambda t, r: t + f'    alpha_VRAM_END {operator} 16;\n')

    def test_multiline_redefinition(self):
        self.reject(lambda t, r: t + '    alpha_VRAM_END\n        = .;\n')

    def test_non_dot_definition(self):
        self.reject(lambda t, r: t.replace('alpha_VRAM_END = .;', 'alpha_VRAM_END = 0x80200080;'))

    def test_cyclic_alias(self):
        self.reject(lambda t, r: t.replace('alpha_VRAM_END = .;',
                    'alpha_VRAM_END = beta_VRAM_END;'))

    def test_non_immediate_predecessor(self):
        self.reject(lambda t, r: t.replace('    beta_ROM_START = __romPos;',
                    self.block('middle', '0x80300000', r) + '    beta_ROM_START = __romPos;'))

    def test_position_change_after_end(self):
        self.reject(lambda t, r: t.replace('alpha_VRAM_END = .;',
                                           'alpha_VRAM_END = .;\n    . += 16;'))

    def test_alignment_after_end(self):
        self.reject(lambda t, r: t.replace('alpha_VRAM_END = .;',
                                           'alpha_VRAM_END = .;\n    . = ALIGN(., 16);'))

    def test_end_before_bss(self):
        self.reject(lambda t, r: t.replace('    alpha_VRAM_END = .;\n', '').replace(
            '    alpha_bss_VRAM =', '    alpha_VRAM_END = .;\n    alpha_bss_VRAM ='))

    def test_end_inside_body(self):
        self.reject(lambda t, r: t.replace('    alpha_VRAM_END = .;\n', '').replace(
            f'        ver/{r}/build/src/alpha.c.o(.bss);',
            f'        ver/{r}/build/src/alpha.c.o(.bss);\n    alpha_VRAM_END = .;'))

    def test_symbolic_arithmetic(self):
        self.reject(lambda t, r: t.replace('.beta alpha_VRAM_END :',
                                           '.beta alpha_VRAM_END + 16 :'))

    def test_symbolic_function(self):
        self.reject(lambda t, r: t.replace('.beta alpha_VRAM_END :',
                                           '.beta ALIGN(alpha_VRAM_END, 16) :'))

    def test_invalid_alignment(self):
        self.reject(lambda t, r: t.replace('. = ALIGN(., 16);', '. = ALIGN(., 3);'))

    def test_missing_body_close(self):
        self.reject(lambda t, r: t.replace('    }\n', '', 1))

    def test_unrelated_noload(self):
        self.reject(lambda t, r: t.replace('.alpha_bss (NOLOAD)', '.foreign_bss (NOLOAD)'))


if __name__ == '__main__':
    unittest.main()
