import io
import stat
import unittest
import zipfile
import pal_release_gates as gate

class PackageTests(unittest.TestCase):
    def archive(self, extra=(), omit=(), duplicate=None, mode_override=None):
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w') as z:
            for name in list(gate.PACKAGE_MODES) + list(gate.PACKAGE_DIRS) + list(extra):
                if name in omit: continue
                row = zipfile.ZipInfo(name)
                mode = (stat.S_IFDIR | 0o755) if name in gate.PACKAGE_DIRS else (stat.S_IFREG | gate.PACKAGE_MODES.get(name, 0o644))
                if mode_override and name == mode_override[0]: mode = mode_override[1]
                row.external_attr = mode << 16
                z.writestr(row, b'')
            if duplicate:
                z.writestr(duplicate, b'')
        data.seek(0)
        return zipfile.ZipFile(data)

    def test_valid(self):
        with self.archive() as z: gate.validate_package(z)

    def test_unexpected_rom_save_and_guide(self):
        for name in ('App/PaperMarioPAL/rom.z64', 'App/PaperMarioPAL/data/pm64.sav', 'App/PaperMarioPAL/paper_mario_v4.tsv', '../escape'):
            with self.subTest(name=name), self.archive(extra=[name]) as z, self.assertRaises(AssertionError): gate.validate_package(z)

    def test_missing_entry(self):
        with self.archive(omit=[gate.APP+'pm64']) as z, self.assertRaises(AssertionError): gate.validate_package(z)

    def test_duplicate_and_symlink_rejected(self):
        import warnings
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', UserWarning)
            with self.archive(duplicate=gate.APP+'pm64') as z, self.assertRaises(AssertionError): gate.validate_package(z)
        with self.archive(mode_override=(gate.APP+'pm64', stat.S_IFLNK | 0o777)) as z, self.assertRaises(AssertionError): gate.validate_package(z)

if __name__ == '__main__': unittest.main()
