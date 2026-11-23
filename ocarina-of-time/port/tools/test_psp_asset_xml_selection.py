import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from tools import psp_port_asset_segments as segments


def load_offsets(version):
    context = segments.NativeAssetContext.__new__(segments.NativeAssetContext)
    context.version = version
    context.segment_ids = {}
    context.xml_symbol_offsets = {}
    context.texture_ranges_by_segment = {}
    context._load_xml_offsets()
    return context


class XmlSelectionTest(unittest.TestCase):
    def test_version_selects_offsets_and_texture_ranges(self):
        pal = load_offsets("pal-1.0")
        ntsc = load_offsets("ntsc-1.0")
        self.assertEqual(pal.xml_symbol_offsets["gameplay_keep"]["gCircleGlowSRTex"], 0x555B0)
        self.assertEqual(ntsc.xml_symbol_offsets["gameplay_keep"]["gCircleGlowSRTex"], 0x5AAE0)
        self.assertIn((0x555B0, 0x55DB0), pal.texture_ranges_by_segment["gameplay_keep"])
        self.assertNotIn((0x5AAE0, 0x5B2E0), pal.texture_ranges_by_segment["gameplay_keep"])

    def test_selected_conflicting_symbols_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            paths = [Path("assets/xml/objects/a.xml"), Path("assets/xml/objects/b.xml")]
            for path, offset in zip(paths, ("0x10", "0x20")):
                (root / path).parent.mkdir(parents=True, exist_ok=True)
                (root / path).write_text(
                    f'<Root><File Name="same"><Array Name="duplicate" Offset="{offset}"/></File></Root>'
                )
            config = SimpleNamespace(assets=[SimpleNamespace(xml_path=p) for p in paths])
            with patch.object(segments, "ROOT", root), patch.object(
                segments.version_config, "load_version_config", return_value=config
            ):
                with self.assertRaisesRegex(ValueError, "conflicting offsets.*same/duplicate"):
                    load_offsets("pal-1.0")


if __name__ == "__main__":
    unittest.main()
