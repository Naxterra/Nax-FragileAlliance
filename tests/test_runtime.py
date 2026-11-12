"""Tests of the runtime's file layer and CD image reader, run through tests/cpp/test_runtime.cpp.

They need the Visual Studio C++ build tools and are skipped when they are missing. No game files are needed;
the CD image is a small ISO 9660 image built below.
"""
import os
import shutil
import struct
import subprocess
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
VSWHERE = os.path.join(os.environ.get("ProgramFiles(x86)", ""), "Microsoft Visual Studio", "Installer", "vswhere.exe")
HAVE_COMPILER = os.path.exists(VSWHERE) and shutil.which("pwsh") is not None
FOLDER = os.path.join(ROOT, "build", "tests", "runtime")
EXE = os.path.join(FOLDER, "test_runtime.exe")


def both16(value):
    return struct.pack("<H", value) + struct.pack(">H", value)


def both32(value):
    return struct.pack("<I", value) + struct.pack(">I", value)


def record(name: bytes, extent: int, size: int, directory: bool) -> bytes:
    body = bytes([0]) + both32(extent) + both32(size) + bytes(7) + bytes([2 if directory else 0, 0, 0])
    body += both16(1) + bytes([len(name)]) + name
    if len(name) % 2 == 0:
        body += b"\0"
    return bytes([len(body) + 1]) + body


def tiny_iso() -> bytes:
    """Sector 16 volume descriptor, 17 terminator, 18 root, 19 DATA directory, 20 the file."""
    content = b"hello world"
    root = record(b"\0", 18, 2048, True) + record(b"\1", 18, 2048, True) + record(b"DATA", 19, 2048, True)
    data = record(b"\0", 19, 2048, True) + record(b"\1", 18, 2048, True) + record(b"README.TXT;1", 20, len(content), False)
    pvd = bytearray(2048)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[156:156 + 34] = record(b"\0", 18, 2048, True)
    terminator = bytearray(2048)
    terminator[0] = 255
    terminator[1:6] = b"CD001"
    image = bytes(16 * 2048) + bytes(pvd) + bytes(terminator)
    image += root.ljust(2048, b"\0") + data.ljust(2048, b"\0") + content.ljust(2048, b"\0")
    return image


@unittest.skipUnless(HAVE_COMPILER, "the Visual Studio C++ build tools are not installed")
class RuntimeFileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.makedirs(FOLDER, exist_ok=True)
        sources = [os.path.join(ROOT, "runtime", "files.cpp"), os.path.join(ROOT, "runtime", "iso9660.cpp"),
                   os.path.join(ROOT, "tests", "cpp", "test_runtime.cpp")]
        quoted = " ".join(f"'{s}'" for s in sources)
        command = (f". '{os.path.join(ROOT, 'tools', 'vsenv.ps1')}' *> $null; "
                   f"cl.exe /nologo /std:c++20 /EHsc /MT /W3 /Od /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE "
                   f"'/I{ROOT}' '/Fo{FOLDER}\\\\' '/Fe{EXE}' {quoted}; exit $LASTEXITCODE")
        built = subprocess.run(["pwsh", "-NoProfile", "-Command", command], capture_output=True, text=True)
        if built.returncode != 0:
            raise AssertionError(f"compiling the runtime tests failed:\n{built.stdout}\n{built.stderr}")
        cls.image = os.path.join(FOLDER, "tiny.iso")
        with open(cls.image, "wb") as f:
            f.write(tiny_iso())

    def run_case(self, name):
        work = os.path.join(FOLDER, "work", name)
        ran = subprocess.run([EXE, name, work, self.image], capture_output=True, text=True, timeout=60)
        self.assertEqual(ran.returncode, 0, ran.stdout)

    def test_parent_directories_never_leave_the_drive(self):
        self.run_case("parent_directories_never_leave_the_drive")

    def test_write_goes_to_user_folder_and_original_stays(self):
        self.run_case("write_goes_to_user_folder_and_original_stays")

    def test_interrupted_write_keeps_previous_version(self):
        self.run_case("interrupted_write_keeps_previous_version")

    def test_write_is_invisible_until_close(self):
        self.run_case("write_is_invisible_until_close")

    def test_leftover_temporary_files_are_removed(self):
        self.run_case("leftover_temporary_files_are_removed")

    def test_missing_file_reports_dos_error_2(self):
        self.run_case("missing_file_reports_dos_error_2")

    def test_find_lists_both_layers_without_temporary_files(self):
        self.run_case("find_lists_both_layers_without_temporary_files")

    def test_removing_an_original_file_is_refused(self):
        self.run_case("removing_an_original_file_is_refused")

    def test_cd_image_files_are_read_and_listed(self):
        self.run_case("cd_image_files_are_read_and_listed")


if __name__ == "__main__":
    unittest.main()
