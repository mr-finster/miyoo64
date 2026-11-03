"""Provider checks with package mocks: no downloads, compiler, ROM or assets."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import prepare_splat_provider as provider

class ProviderTests(unittest.TestCase):
    def test_version_is_checked_before_writing(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(provider.metadata,'version',return_value='other'):
            with self.assertRaises(RuntimeError):provider.prepare(tmp)
            self.assertEqual(list(Path(tmp).iterdir()),[])
    def test_missing_import_is_not_treated_as_a_provider(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(provider.metadata,'version',return_value='0.31.0'), patch.object(provider,'import_module',side_effect=ImportError):
            with self.assertRaises(ImportError):provider.prepare(tmp)
            self.assertEqual(list(Path(tmp).iterdir()),[])
    def test_valid_provider_is_idempotent(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(provider.metadata,'version',return_value='0.31.0'), patch.object(provider,'import_module'):
            path=provider.prepare(tmp);first=path.read_bytes();stamp=path.stat().st_mtime_ns
            self.assertEqual(provider.prepare(tmp),path)
            self.assertEqual(path.read_bytes(),first);self.assertEqual(path.stat().st_mtime_ns,stamp)
    def test_other_adapter_is_preserved(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(provider.metadata,'version',return_value='0.31.0'), patch.object(provider,'import_module'):
            path=Path(tmp)/'tools/splat/Makefile';path.parent.mkdir(parents=True);path.write_text('another provider\n')
            with self.assertRaises(FileExistsError):provider.prepare(tmp)
            self.assertEqual(path.read_text(),'another provider\n')
if __name__=='__main__':unittest.main()
