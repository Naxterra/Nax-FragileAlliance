"""Tests of the live AI upscaler, run through tests/cpp/test_neural.cpp.

They need the Visual Studio C++ build tools and are skipped when they are missing. They run on the graphics card, or on WARP without one, and need neither Upscayl nor game files: each case writes a small random model of the same layout into its own folder under build/tests/neural.
"""
import os
import shutil
import subprocess
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
VSWHERE = os.path.join(os.environ.get("ProgramFiles(x86)", ""), "Microsoft Visual Studio", "Installer", "vswhere.exe")
HAVE_COMPILER = os.path.exists(VSWHERE) and shutil.which("pwsh") is not None
FOLDER = os.path.join(ROOT, "build", "tests", "neural")
EXE = os.path.join(FOLDER, "test_neural.exe")


@unittest.skipUnless(HAVE_COMPILER, "the Visual Studio C++ build tools are not installed")
class NeuralTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.makedirs(FOLDER, exist_ok=True)
        sources = [os.path.join(ROOT, "runtime", name) for name in ("neural.cpp",)]
        sources.append(os.path.join(ROOT, "tests", "cpp", "test_neural.cpp"))
        quoted = " ".join(f"'{s}'" for s in sources)
        command = (f". '{os.path.join(ROOT, 'tools', 'vsenv.ps1')}' *> $null; "
                   f"cl.exe /nologo /std:c++20 /EHsc /MT /W3 /Od /DNOMINMAX /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS "
                   f"'/I{ROOT}' '/Fo{FOLDER}\\\\' '/Fe{EXE}' {quoted} d3d12.lib dxgi.lib d3dcompiler.lib; exit $LASTEXITCODE")
        built = subprocess.run(["pwsh", "-NoProfile", "-Command", command], capture_output=True, text=True, errors="replace")
        if built.returncode != 0:
            raise AssertionError(f"compiling the upscaler tests failed:\n{built.stdout}\n{built.stderr}")

    def run_case(self, name):
        ran = subprocess.run([EXE, name, os.path.join(FOLDER, name)], capture_output=True, text=True, timeout=300)
        self.assertEqual(ran.returncode, 0, ran.stdout)

    def test_gpu_result_matches_the_cpu_network(self):
        self.run_case("gpu_result_matches_the_cpu_network")

    def test_changed_area_alone_gives_the_same_picture_as_a_full_run(self):
        self.run_case("changed_area_alone_gives_the_same_picture_as_a_full_run")

    def test_model_with_half_precision_weights_loads(self):
        self.run_case("model_with_half_precision_weights_loads")

    def test_model_of_another_layout_is_refused(self):
        self.run_case("model_of_another_layout_is_refused")


if __name__ == "__main__":
    unittest.main()
