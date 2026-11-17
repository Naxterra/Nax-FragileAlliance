"""Tests of the texture pack and the dump of original pictures, run through tests/cpp/test_textures.cpp.

They need the Visual Studio C++ build tools and are skipped when they are missing. No game files are needed:
each case works in its own folder under build/tests/textures.
"""
import os
import shutil
import subprocess
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
VSWHERE = os.path.join(os.environ.get("ProgramFiles(x86)", ""), "Microsoft Visual Studio", "Installer", "vswhere.exe")
HAVE_COMPILER = os.path.exists(VSWHERE) and shutil.which("pwsh") is not None
FOLDER = os.path.join(ROOT, "build", "tests", "textures")
EXE = os.path.join(FOLDER, "test_textures.exe")


@unittest.skipUnless(HAVE_COMPILER, "the Visual Studio C++ build tools are not installed")
class TextureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.makedirs(FOLDER, exist_ok=True)
        sources = [os.path.join(ROOT, "runtime", name) for name in ("textures.cpp", "image.cpp")]
        sources.append(os.path.join(ROOT, "tests", "cpp", "test_textures.cpp"))
        quoted = " ".join(f"'{s}'" for s in sources)
        command = (f". '{os.path.join(ROOT, 'tools', 'vsenv.ps1')}' *> $null; "
                   f"cl.exe /nologo /std:c++20 /EHsc /MT /W3 /Od /DNOMINMAX /DUNICODE /D_UNICODE "
                   f"'/I{ROOT}' '/Fo{FOLDER}\\\\' '/Fe{EXE}' {quoted} ole32.lib windowscodecs.lib; exit $LASTEXITCODE")
        built = subprocess.run(["pwsh", "-NoProfile", "-Command", command], capture_output=True, text=True, errors="replace")
        if built.returncode != 0:
            raise AssertionError(f"compiling the texture tests failed:\n{built.stdout}\n{built.stderr}")

    def run_case(self, name):
        ran = subprocess.run([EXE, name, os.path.join(FOLDER, name)], capture_output=True, text=True, timeout=60)
        self.assertEqual(ran.returncode, 0, ran.stdout)

    def test_dumped_picture_keeps_its_colours_and_palette(self):
        self.run_case("dumped_picture_keeps_its_colours_and_palette")

    def test_picture_is_not_dumped_while_the_palette_changes(self):
        self.run_case("picture_is_not_dumped_while_the_palette_changes")

    def test_pack_picture_replaces_the_original(self):
        self.run_case("pack_picture_replaces_the_original")

    def test_missing_picture_is_never_decoded_while_absent(self):
        self.run_case("missing_picture_is_never_decoded_while_absent")

    def test_broken_pack_picture_leaves_the_original(self):
        self.run_case("broken_pack_picture_leaves_the_original")

    def test_picture_added_to_the_pack_later_is_picked_up(self):
        self.run_case("picture_added_to_the_pack_later_is_picked_up")

    def test_without_a_pack_folder_nothing_is_created_or_done(self):
        self.run_case("without_a_pack_folder_nothing_is_created_or_done")

    def test_legacy_repaint_is_ignored_until_faithful_rebuild(self):
        self.run_case("legacy_repaint_is_ignored_until_faithful_rebuild")


if __name__ == "__main__":
    unittest.main()
