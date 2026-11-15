"""Tests of the software mixer that plays the game's samples, run through tests/cpp/test_mixer.cpp.

They need the Visual Studio C++ build tools and are skipped when they are missing. No sound device and no game
files are needed: the mixer writes into memory.
"""
import os
import shutil
import subprocess
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
VSWHERE = os.path.join(os.environ.get("ProgramFiles(x86)", ""), "Microsoft Visual Studio", "Installer", "vswhere.exe")
HAVE_COMPILER = os.path.exists(VSWHERE) and shutil.which("pwsh") is not None
FOLDER = os.path.join(ROOT, "build", "tests", "mixer")
EXE = os.path.join(FOLDER, "test_mixer.exe")


@unittest.skipUnless(HAVE_COMPILER, "the Visual Studio C++ build tools are not installed")
class MixerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.makedirs(FOLDER, exist_ok=True)
        sources = [os.path.join(ROOT, "runtime", "mixer.cpp"), os.path.join(ROOT, "tests", "cpp", "test_mixer.cpp")]
        quoted = " ".join(f"'{s}'" for s in sources)
        command = (f". '{os.path.join(ROOT, 'tools', 'vsenv.ps1')}' *> $null; "
                   f"cl.exe /nologo /std:c++20 /EHsc /MT /W3 /Od /DNOMINMAX "
                   f"'/I{ROOT}' '/Fo{FOLDER}\\\\' '/Fe{EXE}' {quoted}; exit $LASTEXITCODE")
        built = subprocess.run(["pwsh", "-NoProfile", "-Command", command], capture_output=True, text=True)
        if built.returncode != 0:
            raise AssertionError(f"compiling the mixer tests failed:\n{built.stdout}\n{built.stderr}")

    def run_case(self, name):
        ran = subprocess.run([EXE, name], capture_output=True, text=True, timeout=60)
        self.assertEqual(ran.returncode, 0, ran.stdout)

    def test_eight_and_sixteen_bit_samples_play_at_the_same_level(self):
        self.run_case("eight_and_sixteen_bit_samples_play_at_the_same_level")

    def test_next_buffer_given_at_the_end_plays_without_a_gap(self):
        self.run_case("next_buffer_given_at_the_end_plays_without_a_gap")

    def test_voice_without_a_next_buffer_ends_and_is_reported_once(self):
        self.run_case("voice_without_a_next_buffer_ends_and_is_reported_once")

    def test_stopping_inside_the_buffer_callback_ends_without_a_finish_report(self):
        self.run_case("stopping_inside_the_buffer_callback_ends_without_a_finish_report")

    def test_sample_at_half_the_output_rate_lasts_twice_as_long(self):
        self.run_case("sample_at_half_the_output_rate_lasts_twice_as_long")

    def test_pan_to_the_left_silences_the_right_channel(self):
        self.run_case("pan_to_the_left_silences_the_right_channel")

    def test_interleaved_stereo_keeps_its_channels_apart(self):
        self.run_case("interleaved_stereo_keeps_its_channels_apart")

    def test_empty_buffers_handed_over_forever_do_not_stop_the_mixer(self):
        self.run_case("empty_buffers_handed_over_forever_do_not_stop_the_mixer")

    def test_loud_voices_together_are_clipped_not_wrapped(self):
        self.run_case("loud_voices_together_are_clipped_not_wrapped")


if __name__ == "__main__":
    unittest.main()
