"""Synthetic-only tests: python3 -B -m unittest discover -s tools/port -p test_rom_segments.py -v."""
import json
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import yaml

import rom_segments


US = {'segments': [
    {'name': 'header', 'type': 'header', 'start': 0},
    {'dir': 'battle/area/test', 'type': 'code', 'start': 64},
    [128, 'pm_msg'],
    [256],
]}
PAL = {'segments': [
    [0, 'header', 'header'],
    {'start': 64, 'type': 'code', 'name': 'main'},
    *[{'start': 128 + index * 64, 'type': 'pm_msg', 'name': 'msg_pal_' + language}
      for index, language in enumerate(('en', 'de', 'fr', 'es'))],
    [384],
]}
EXPECTED_US = [
    {'name': 'header', 'type': 'header', 'rom_start': 0, 'rom_end': 64},
    {'name': 'battle_area_test', 'type': 'code', 'rom_start': 64, 'rom_end': 128},
    {'name': 'msg', 'type': 'pm_msg', 'rom_start': 128, 'rom_end': 256},
    {'name': None, 'type': None, 'rom_start': 256, 'rom_end': None},
]


class RomSegmentsTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory(prefix='pal_segments_test_')))
        # Run the real CLI with an isolated project root and synthetic refs only.
        self.script = self.root / 'tools/port/rom_segments.py'
        self.script.parent.mkdir(parents=True)
        shutil.copyfile(Path(rom_segments.__file__), self.script)
        for region, cfg in (('us', US), ('pal', PAL)):
            path = self.config(region)
            path.parent.mkdir(parents=True)
            path.write_text(yaml.safe_dump(cfg), encoding='utf-8')
        self.us_output = self.root / 'build/game/rom_segments.json'
        self.pal_output = self.root / 'build/game/pal/rom_segments.json'

    def config(self, region):
        return self.root / 'refs/papermario/ver' / region / 'splat.yaml'

    def cli(self, *args, cwd=None, umask=-1):
        return subprocess.run([sys.executable, '-B', str(self.script), *map(str, args)],
                              cwd=cwd or self.root, umask=umask,
                              capture_output=True, text=True, timeout=10)

    def seed(self, output):
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(b'previous complete result')

    def assert_preserved(self, output):
        self.assertEqual(output.read_bytes(), b'previous complete result')
        self.assertEqual(list(output.parent.glob('.rom_segments-*.tmp')), [])

    def test_legacy_no_arguments_exact_json_and_summary(self):
        result = self.cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.us_output.read_bytes(), json.dumps(EXPECTED_US, indent=0).encode())
        self.assertEqual(result.stdout, 'rom_segments.py: 4 segments, ROM 0x0..0x100\n')
        self.assertEqual(result.stderr, '')
        self.assertFalse(self.pal_output.exists())

    def test_explicit_us_matches_legacy(self):
        self.assertEqual(self.cli().returncode, 0)
        legacy = self.us_output.read_bytes()
        result = self.cli('--region', 'us')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.us_output.read_bytes(), legacy)

    def test_pal_languages_order_and_ranges(self):
        result = self.cli('--region', 'pal')
        self.assertEqual(result.returncode, 0, result.stderr)
        segments = json.loads(self.pal_output.read_text())
        self.assertEqual([segment['name'] for segment in segments],
                         ['header', 'main', 'msg_pal_en', 'msg_pal_de', 'msg_pal_fr', 'msg_pal_es', None])
        self.assertEqual([segment['rom_start'] for segment in segments], [0, 64, 128, 192, 256, 320, 384])
        self.assertEqual([segment['rom_end'] for segment in segments], [64, 128, 192, 256, 320, 384, None])

    def test_only_selected_region_is_loaded(self):
        for selected, other in (('us', 'pal'), ('pal', 'us')):
            with self.subTest(region=selected):
                original = self.config(other).read_text()
                self.config(other).write_text('segments: [broken yaml')
                result = self.cli('--region', selected)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.config(other).write_text(original)

    def test_default_destinations_are_separate(self):
        self.assertEqual(self.cli().returncode, 0)
        us = self.us_output.read_bytes()
        self.assertEqual(self.cli('--region', 'pal').returncode, 0)
        pal = self.pal_output.read_bytes()
        self.assertEqual(self.us_output.read_bytes(), us)
        self.assertEqual(self.cli().returncode, 0)
        self.assertEqual(self.pal_output.read_bytes(), pal)

    def test_custom_output_for_both_regions(self):
        for region in ('us', 'pal'):
            with self.subTest(region=region):
                destination = self.root / 'custom' / region / 'segments.json'
                result = self.cli('--region', region, '--output', destination)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(destination.is_file())
        self.assertFalse(self.us_output.exists())
        self.assertFalse(self.pal_output.exists())

    def test_relative_output_and_short_option(self):
        result = self.cli('--region', 'pal', '-o', 'custom/pal.json')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.root / 'custom/pal.json').is_file())

    def test_pal_cannot_target_us_default(self):
        self.seed(self.us_output)
        for destination in (self.us_output, 'build/game/rom_segments.json',
                            'build/game/pal/../rom_segments.json'):
            with self.subTest(destination=destination):
                result = self.cli('--region', 'pal', '-o', destination)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('PAL output must not overwrite', result.stderr)
                self.assert_preserved(self.us_output)

    def test_pal_cannot_target_us_through_symlink(self):
        self.seed(self.us_output)
        alias = self.root / 'alias'
        alias.symlink_to(self.us_output.parent, target_is_directory=True)
        result = self.cli('--region', 'pal', '-o', alias / self.us_output.name)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('PAL output must not overwrite', result.stderr)
        self.assert_preserved(self.us_output)

    def test_unknown_region_rejected_without_output_change(self):
        self.seed(self.us_output)
        result = self.cli('--region', 'jp')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('invalid choice', result.stderr)
        self.assertIn('us', result.stderr)
        self.assertIn('pal', result.stderr)
        self.assert_preserved(self.us_output)

    def test_us_default_preserves_existing_file_symlink(self):
        target = self.root / 'existing.json'
        self.seed(target)
        self.us_output.parent.mkdir(parents=True)
        self.us_output.symlink_to(target)
        result = self.cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.us_output.is_symlink())
        self.assertEqual(self.us_output.readlink(), target)
        self.assertEqual(target.read_bytes(), json.dumps(EXPECTED_US, indent=0).encode())

    def test_relative_file_symlink_is_preserved(self):
        target = self.root / 'existing.json'
        self.seed(target)
        self.us_output.parent.mkdir(parents=True)
        self.us_output.symlink_to('../../existing.json')
        result = self.cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.us_output.readlink(), Path('../../existing.json'))
        self.assertEqual(json.loads(target.read_text()), EXPECTED_US)

    def test_relative_symlink_chain_is_preserved(self):
        target = self.root / 'existing.json'
        self.seed(target)
        middle = self.root / 'links/middle.json'
        middle.parent.mkdir()
        middle.symlink_to('../existing.json')
        self.us_output.parent.mkdir(parents=True)
        self.us_output.symlink_to('../../links/middle.json')
        result = self.cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.us_output.readlink(), Path('../../links/middle.json'))
        self.assertEqual(middle.readlink(), Path('../existing.json'))
        self.assertEqual(json.loads(target.read_text()), EXPECTED_US)

    def test_dangling_file_symlink_creates_target_and_preserves_link(self):
        target = self.root / 'new.json'
        self.us_output.parent.mkdir(parents=True)
        self.us_output.symlink_to('../../new.json')
        result = self.cli(umask=0o077)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.us_output.readlink(), Path('../../new.json'))
        self.assertEqual(json.loads(target.read_text()), EXPECTED_US)
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o600)

    def test_dangling_symlink_chain_creates_only_final_target(self):
        middle = self.root / 'middle.json'
        target = self.root / 'new.json'
        middle.symlink_to('new.json')
        self.us_output.parent.mkdir(parents=True)
        self.us_output.symlink_to('../../middle.json')
        result = self.cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.us_output.is_symlink())
        self.assertEqual(middle.readlink(), Path('new.json'))
        self.assertEqual(json.loads(target.read_text()), EXPECTED_US)

    def test_dangling_target_parent_is_not_created(self):
        self.us_output.parent.mkdir(parents=True)
        self.us_output.symlink_to('../../missing/target.json')
        unrelated = self.root / 'unrelated.json'
        self.seed(unrelated)
        result = self.cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('output target directory does not exist', result.stderr)
        self.assertEqual(self.us_output.readlink(), Path('../../missing/target.json'))
        self.assertFalse((self.root / 'missing').exists())
        self.assert_preserved(unrelated)

    def test_symlink_loop_is_reported_without_changes(self):
        self.us_output.parent.mkdir(parents=True)
        self.us_output.symlink_to(self.us_output.name)
        result = self.cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('Traceback', result.stderr)
        self.assertIn('loop', result.stderr.lower())
        self.assertEqual(self.us_output.readlink(), Path(self.us_output.name))
        self.assertEqual(list(self.us_output.parent.glob('.rom_segments-*.tmp')), [])

    def test_pal_file_symlink_to_us_is_rejected(self):
        self.seed(self.us_output)
        alias = self.root / 'pal-alias.json'
        alias.symlink_to('build/game/rom_segments.json')
        result = self.cli('--region', 'pal', '-o', alias)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('PAL output must not overwrite', result.stderr)
        self.assertTrue(alias.is_symlink())
        self.assert_preserved(self.us_output)

    def test_pal_and_us_symlink_chains_to_same_target_are_rejected(self):
        target = self.root / 'shared.json'
        self.seed(target)
        self.us_output.parent.mkdir(parents=True)
        self.us_output.symlink_to('../../shared.json')
        alias = self.root / 'pal-alias.json'
        alias.symlink_to('build/game/rom_segments.json')
        for destination in (alias, target):
            with self.subTest(destination=destination):
                result = self.cli('--region', 'pal', '-o', destination)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('PAL output must not overwrite', result.stderr)
                self.assertTrue(alias.is_symlink())
                self.assertTrue(self.us_output.is_symlink())
                self.assert_preserved(target)

    def test_pal_cannot_create_us_target_through_dangling_symlink(self):
        alias = self.root / 'pal-alias.json'
        alias.symlink_to('build/game/rom_segments.json')
        result = self.cli('--region', 'pal', '-o', alias)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('PAL output must not overwrite', result.stderr)
        self.assertTrue(alias.is_symlink())
        self.assertFalse(self.us_output.parent.exists())

    def test_other_working_directory_keeps_default_root_and_relative_custom_output(self):
        elsewhere = self.root / 'elsewhere'
        elsewhere.mkdir()
        result = self.cli(cwd=elsewhere)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(self.us_output.read_text()), EXPECTED_US)
        result = self.cli('--region', 'pal', '-o', 'pal.json', cwd=elsewhere)
        self.assertEqual(result.returncode, 0, result.stderr)
        names = {segment['name'] for segment in json.loads((elsewhere / 'pal.json').read_text())}
        self.assertIn('msg_pal_de', names)
        self.assertFalse(self.pal_output.exists())

    def test_new_output_respects_umask(self):
        for mask in (0o077, 0o027, 0o002):
            with self.subTest(umask=oct(mask)):
                target = self.root / f'mode-{mask}.json'
                result = self.cli('-o', target, umask=mask)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o666 & ~mask)

    def test_existing_regular_target_preserves_permissions(self):
        for mode in (0o600, 0o640, 0o664):
            with self.subTest(mode=oct(mode)):
                target = self.root / f'existing-{mode}.json'
                self.seed(target)
                target.chmod(mode)
                result = self.cli('-o', target, umask=0o077)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(stat.S_IMODE(target.stat().st_mode), mode)
                self.assertEqual(json.loads(target.read_text()), EXPECTED_US)

    def test_symlink_target_preserves_permissions(self):
        target = self.root / 'existing.json'
        self.seed(target)
        target.chmod(0o640)
        self.us_output.parent.mkdir(parents=True)
        self.us_output.symlink_to(target)
        result = self.cli(umask=0o077)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.us_output.is_symlink())
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o640)

    def test_chmod_failure_preserves_regular_target_and_cleans_temporary(self):
        self.seed(self.us_output)
        self.us_output.chmod(0o640)
        with patch.object(rom_segments.os, 'fchmod', side_effect=OSError('synthetic chmod failure')):
            with self.assertRaisesRegex(OSError, 'synthetic chmod failure'):
                rom_segments.write_atomic(self.us_output, EXPECTED_US)
        self.assert_preserved(self.us_output)
        self.assertEqual(stat.S_IMODE(self.us_output.stat().st_mode), 0o640)

    def test_write_chmod_and_replace_failures_preserve_symlink_and_target(self):
        target = self.root / 'existing.json'
        self.seed(target)
        target.chmod(0o640)
        self.us_output.parent.mkdir(parents=True)
        self.us_output.symlink_to('../../existing.json')

        def fail_dump(data, stream, **kwargs):
            stream.write('partial JSON')
            raise OSError('synthetic partial write')

        for owner, name, failure in ((rom_segments.json, 'dump', fail_dump),
                                     (rom_segments.os, 'fchmod', OSError('synthetic chmod failure')),
                                     (rom_segments.os, 'replace', OSError('synthetic replace failure'))):
            with self.subTest(operation=name), patch.object(owner, name, side_effect=failure):
                with self.assertRaises(OSError):
                    rom_segments.write_atomic(self.us_output, EXPECTED_US)
            self.assertEqual(self.us_output.readlink(), Path('../../existing.json'))
            self.assert_preserved(target)
            self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o640)

    def test_temporary_name_collision_does_not_touch_other_file(self):
        self.seed(self.us_output)
        collision = self.us_output.parent / '.rom_segments-collision.tmp'
        collision.write_bytes(b'unrelated file')
        with patch.object(rom_segments.secrets, 'token_hex', return_value='collision'):
            with self.assertRaises(FileExistsError):
                rom_segments.write_atomic(self.us_output, EXPECTED_US)
        self.assertEqual(collision.read_bytes(), b'unrelated file')
        self.assertEqual(self.us_output.read_bytes(), b'previous complete result')

    def test_symlink_to_directory_is_rejected_and_preserved(self):
        directory = self.root / 'directory'
        directory.mkdir()
        self.us_output.parent.mkdir(parents=True)
        self.us_output.symlink_to(directory, target_is_directory=True)
        result = self.cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('output target must be a regular file', result.stderr)
        self.assertTrue(self.us_output.is_symlink())
        self.assertEqual(list(directory.iterdir()), [])

    def test_unresolved_starts_are_skipped_without_reordering(self):
        cfg = {'segments': [[0, 'header'], ['auto', 'code', 'unresolved'],
                            {'type': 'bin'}, [64, 'bin', 'last']]}
        segments = rom_segments.segment_ranges(cfg)
        self.assertEqual([segment['rom_start'] for segment in segments], [0, 64])
        self.assertEqual([segment['rom_end'] for segment in segments], [64, None])

    def test_naming_precedence_and_terminal_marker(self):
        segments = rom_segments.segment_ranges({'segments': [
            {'start': 0, 'name': 'explicit/name', 'dir': 'ignored', 'type': 'code'},
            {'start': 64, 'dir': 'dir/name', 'type': 'bin'}, [128, 'pm_msg'], [192]]})
        self.assertEqual([segment['name'] for segment in segments],
                         ['explicit_name', 'dir_name', 'msg', None])
        self.assertIsNone(segments[-1]['rom_end'])

    def test_zero_length_rom_segments_preserve_order_and_boundaries(self):
        # Synthetic BSS-only segments model the shared starts in US and PAL Splat.
        cfg = {'segments': [
            {'start': 0, 'name': 'heaps', 'type': 'code'},
            {'start': 0, 'name': 'heaps4', 'type': 'code'},
            {'start': 0, 'name': 'evt', 'type': 'code'}, [64]]}
        segments = rom_segments.segment_ranges(cfg)
        self.assertEqual([segment['name'] for segment in segments], ['heaps', 'heaps4', 'evt', None])
        self.assertEqual([segment['rom_start'] for segment in segments], [0, 0, 0, 64])
        self.assertEqual([segment['rom_end'] for segment in segments], [0, 0, 64, None])

    def test_invalid_boundaries_rejected_before_replacement(self):
        self.seed(self.us_output)
        for starts in ([64, 0], [-1, 64], [0, 1.5], [False, 64], [0, 'bad']):
            with self.subTest(starts=starts):
                self.config('us').write_text(yaml.safe_dump({'segments': [[start, 'bin'] for start in starts]}))
                result = self.cli()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('segment', result.stderr)
                self.assert_preserved(self.us_output)

    def test_invalid_configuration_does_not_create_destination(self):
        for cfg in (None, [], {}, {'segments': None}, {'segments': []},
                    {'segments': [[]]}, {'segments': [42]},
                    {'segments': [{'start': 0, 'type': []}]},
                    {'segments': [{'start': 0, 'name': 42}]}, {'segments': [['auto']]}):
            with self.subTest(cfg=cfg):
                self.config('us').write_text(yaml.safe_dump(cfg))
                result = self.cli()
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.us_output.parent.exists())

    def test_missing_selected_config_preserves_result(self):
        self.seed(self.pal_output)
        self.config('pal').unlink()
        result = self.cli('--region', 'pal')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('splat.yaml', result.stderr)
        self.assert_preserved(self.pal_output)

    def test_invalid_yaml_preserves_result(self):
        self.seed(self.us_output)
        self.config('us').write_text('segments: [broken yaml')
        result = self.cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('Traceback', result.stderr)
        self.assert_preserved(self.us_output)

    def test_partial_json_write_preserves_result_and_removes_temporary(self):
        self.seed(self.us_output)

        def fail_dump(data, stream, **kwargs):
            stream.write('partial JSON')
            raise OSError('synthetic write failure')

        with patch.object(rom_segments.json, 'dump', side_effect=fail_dump):
            with self.assertRaises(OSError):
                rom_segments.write_atomic(self.us_output, EXPECTED_US)
        self.assert_preserved(self.us_output)

    def test_replace_failure_preserves_result_and_removes_temporary(self):
        self.seed(self.us_output)
        with patch.object(rom_segments.os, 'replace', side_effect=OSError('synthetic replace failure')):
            with self.assertRaises(OSError):
                rom_segments.write_atomic(self.us_output, EXPECTED_US)
        self.assert_preserved(self.us_output)

    def test_flush_failure_preserves_result_and_removes_temporary(self):
        self.seed(self.us_output)
        with patch.object(rom_segments.os, 'fsync', side_effect=OSError('synthetic flush failure')):
            with self.assertRaises(OSError):
                rom_segments.write_atomic(self.us_output, EXPECTED_US)
        self.assert_preserved(self.us_output)

    def test_output_directory_error_is_reported(self):
        blocker = self.root / 'not_a_directory'
        blocker.write_text('keep')
        result = self.cli('-o', blocker / 'output.json')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('Traceback', result.stderr)
        self.assertEqual(blocker.read_text(), 'keep')


if __name__ == '__main__':
    unittest.main()
