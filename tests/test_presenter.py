"""Final DX12 pixel checks, including fades, shrinking and texture reloads."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FOLDER = ROOT / "build" / "tests" / "presenter"
EXE = FOLDER / "test_presenter.exe"
VSWHERE = Path(os.environ.get("ProgramFiles(x86)", "")) / "Microsoft Visual Studio/Installer/vswhere.exe"


@unittest.skipUnless(VSWHERE.is_file() and shutil.which("pwsh"), "Windows C++ build tools are required")
class PresenterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        FOLDER.mkdir(parents=True, exist_ok=True)
        sources = [ROOT / "runtime" / name for name in ("presenter.cpp", "neural.cpp", "image.cpp")]
        sources.append(ROOT / "tests" / "cpp" / "test_presenter.cpp")
        quoted = " ".join(f"'{source}'" for source in sources)
        command = (f". '{ROOT / 'tools/vsenv.ps1'}' *> $null; "
                   f"cl.exe /nologo /std:c++20 /EHsc /MT /O2 /DNOMINMAX /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN "
                   f"'/I{ROOT}' '/Fo{FOLDER}\\\\' '/Fe{EXE}' {quoted} "
                   "user32.lib ole32.lib windowscodecs.lib d3d12.lib dxgi.lib d3dcompiler.lib; exit $LASTEXITCODE")
        made = subprocess.run(["pwsh", "-NoProfile", "-Command", command], capture_output=True, text=True, errors="replace")
        if made.returncode:
            raise AssertionError(f"Compositor test compilation failed:\n{made.stdout}\n{made.stderr}")

    def run_case(self, name):
        with tempfile.TemporaryDirectory(prefix=name + "-", dir=FOLDER) as folder:
            ran = subprocess.run([str(EXE), name, folder], capture_output=True, text=True, timeout=60)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_black_fade(self): self.run_case("black_fade")
    def test_half_fade(self): self.run_case("half_fade")
    def test_palette_animation(self): self.run_case("palette_animation")
    def test_scaled_foreground_occlusion(self): self.run_case("scaled_occlusion")
    def test_scaled_final_source_pixel(self): self.run_case("scaled_visible")
    def test_scaled_transparent_source_pixel(self): self.run_case("scaled_transparency")
    def test_fractional_scaled_position(self): self.run_case("fractional_scale")
    def test_transparent_hole(self): self.run_case("transparent_hole")
    def test_reload_keeps_previous_texture_until_upload(self): self.run_case("reload_budget")


if __name__ == "__main__":
    unittest.main()
