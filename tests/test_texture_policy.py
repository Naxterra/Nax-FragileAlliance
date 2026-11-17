"""Asset selection and output policy without optional image/model dependencies."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "redraw-textures.py"
spec = importlib.util.spec_from_file_location("redraw_textures", SCRIPT)
redraw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(redraw)


class TexturePolicyTests(unittest.TestCase):
    def test_candidate_output_cannot_overwrite_active_artwork(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.assertEqual(redraw.candidate_folder(root), root / "candidates")
            for target in (root, root / "pack", root / "pack" / "nested", root / "dump"):
                with self.assertRaises(ValueError):
                    redraw.candidate_folder(root, target)

    def test_asset_classification_is_explicit(self):
        key = "1" * 16
        self.assertEqual(redraw.asset_kind(key, {}, "art"), "art")
        self.assertEqual(redraw.asset_kind(key, {key: "interface"}, "terrain"), "interface")
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "types.json"
            path.write_text(json.dumps({key: "terrain"}), encoding="utf-8")
            self.assertEqual(redraw.read_asset_types(path), {key: "terrain"})
            path.write_text(json.dumps({key: "guess-from-width"}), encoding="utf-8")
            with self.assertRaises(ValueError):
                redraw.read_asset_types(path)

    def test_interface_artwork_never_reaches_generation(self):
        key = "1" * 16
        args = types.SimpleNamespace(asset_types={key: "interface"}, kind="terrain")
        with self.assertRaisesRegex(ValueError, "faithful enlargement"):
            redraw.redraw(None, key + ".png", "candidate.png", args, "unused")

    def test_bulk_repainting_requires_explicit_asset_selection(self):
        ran = subprocess.run([sys.executable, "-I", str(SCRIPT)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 2)
        self.assertIn("Select texture keys", ran.stderr)


if __name__ == "__main__":
    unittest.main()
