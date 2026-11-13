"""Tests that translate small synthetic 32-bit programs to C++, compile them with the core runtime and run them.

The programs are assembled by hand below, so no game files are needed. The tests need the Visual Studio C++
build tools and are skipped when they are missing.
"""
import os
import shutil
import struct
import subprocess
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "recomp"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import discover32  # noqa: E402
import emit32  # noqa: E402
import le  # noqa: E402
from test_le import CODE_BASE, CODE_FLAGS, DATA_BASE, DATA_FLAGS, bound_program, fixup32  # noqa: E402

VSWHERE = os.path.join(os.environ.get("ProgramFiles(x86)", ""), "Microsoft Visual Studio", "Installer", "vswhere.exe")
HAVE_COMPILER = os.path.exists(VSWHERE) and shutil.which("pwsh") is not None
R = DATA_BASE  # results area


class Asm:
    """A tiny assembler: raw bytes plus labels, relative branches and absolute code addresses with fixups."""

    def __init__(self):
        self.code = bytearray()
        self.labels = {}
        self.relative = []  # (offset of rel32, label)
        self.absolute = []  # (offset of abs32, label)

    def label(self, name):
        self.labels[name] = len(self.code)

    def b(self, *values):
        self.code += bytes(values)

    def d32(self, value):
        self.code += struct.pack("<I", value & 0xFFFFFFFF)

    def rel(self, opcode_bytes, name):
        self.code += bytes(opcode_bytes)
        self.relative.append((len(self.code), name))
        self.d32(0)

    def call(self, name):
        self.rel([0xE8], name)

    def jmp(self, name):
        self.rel([0xE9], name)

    def code_address(self, name):
        self.absolute.append((len(self.code), name))
        self.d32(0)

    def finish(self):
        for at, name in self.relative:
            struct.pack_into("<i", self.code, at, self.labels[name] - (at + 4))
        fixups = b"".join(fixup32(at, 1, self.labels[name]) for at, name in self.absolute)
        return bytes(self.code), fixups


def main_program():
    a = Asm()
    # Flags of ADD: carry and zero, then overflow and sign.
    a.b(0xB8); a.d32(0xFFFFFFFF)
    a.b(0x83, 0xC0, 0x01)
    a.b(0x0F, 0x92, 0x05); a.d32(R + 0)
    a.b(0x0F, 0x94, 0x05); a.d32(R + 1)
    a.b(0xB8); a.d32(0x7FFFFFFF)
    a.b(0x83, 0xC0, 0x01)
    a.b(0x0F, 0x90, 0x05); a.d32(R + 2)
    a.b(0x0F, 0x98, 0x05); a.d32(R + 3)
    # CMP 5, 7: signed and unsigned less.
    a.b(0xB9); a.d32(5)
    a.b(0x83, 0xF9, 0x07)
    a.b(0x0F, 0x9C, 0x05); a.d32(R + 4)
    a.b(0x0F, 0x92, 0x05); a.d32(R + 5)
    # LOOP sums 10 down to 1.
    a.b(0x31, 0xC0)
    a.b(0xB9); a.d32(10)
    a.label("sum"); a.b(0x01, 0xC8); a.b(0xE2, 0xFC)
    a.b(0xA3); a.d32(R + 0x08)
    # A call with two stack arguments and RET 8.
    a.b(0x6A, 0x03); a.b(0x6A, 0x04); a.call("mul"); a.b(0xA3); a.d32(R + 0x0C)
    # A jump table in the data object.
    a.b(0xB8); a.d32(2)
    a.b(0xFF, 0x24, 0x85); a.d32(DATA_BASE + 0x400)
    for number, value in enumerate((100, 200, 300)):
        a.label(f"case{number}"); a.b(0xC7, 0x05); a.d32(R + 0x10); a.d32(value); a.jmp("after_table")
    a.label("after_table")
    # An indirect call through a register loaded with a relocated code address.
    a.b(0xBB); a.code_address("g")
    a.b(0xFF, 0xD3)
    # REP MOVSB copies eight bytes.
    a.b(0xBE); a.d32(DATA_BASE + 0x100)
    a.b(0xBF); a.d32(DATA_BASE + 0x200)
    a.b(0xB9); a.d32(8)
    a.b(0xFC); a.b(0xF3, 0xA4)
    # F2 A5 (REPNE MOVSD), the form Watcom uses for block copies, repeats like REP.
    a.b(0xBE); a.d32(DATA_BASE + 0x100)
    a.b(0xBF); a.d32(DATA_BASE + 0x210)
    a.b(0xB9); a.d32(2)
    a.b(0xF2, 0xA5)
    # IDIV 100 / 7.
    a.b(0xB8); a.d32(100); a.b(0x99); a.b(0xB9); a.d32(7); a.b(0xF7, 0xF9)
    a.b(0xA3); a.d32(R + 0x18); a.b(0x89, 0x15); a.d32(R + 0x1C)
    # x87: 3 * 4, reversed and plain subtraction, and a comparison read through FNSTSW and SAHF.
    a.b(0xDB, 0x05); a.d32(DATA_BASE + 0x300)
    a.b(0xDB, 0x05); a.d32(DATA_BASE + 0x304)
    a.b(0xDE, 0xC9)
    a.b(0xDB, 0x1D); a.d32(R + 0x20)
    for opcode, slot in ((0xE1, 0x24), (0xE9, 0x28)):
        a.b(0xDB, 0x05); a.d32(DATA_BASE + 0x308)
        a.b(0xDB, 0x05); a.d32(DATA_BASE + 0x300)
        a.b(0xDE, opcode)
        a.b(0xDB, 0x1D); a.d32(R + slot)
    a.b(0xDB, 0x05); a.d32(DATA_BASE + 0x308)
    a.b(0xDB, 0x05); a.d32(DATA_BASE + 0x300)
    a.b(0xDE, 0xD9); a.b(0xDF, 0xE0); a.b(0x9E)
    a.b(0x0F, 0x92, 0x05); a.d32(R + 0x2C)
    # Shifts.
    a.b(0xB8); a.d32(0x80000001); a.b(0xD1, 0xE0)
    a.b(0x0F, 0x92, 0x05); a.d32(R + 0x30); a.b(0xA3); a.d32(R + 0x34)
    a.b(0xB8); a.d32(0xFFFFFFF8); a.b(0xC1, 0xF8, 0x02); a.b(0xA3); a.d32(R + 0x38)
    # Partial registers.
    a.b(0xB8); a.d32(0x11223344); a.b(0xB0, 0xFF); a.b(0xB4, 0xEE); a.b(0xA3); a.d32(R + 0x3C)
    # A callee that returns through POP and an indirect jump.
    a.call("h"); a.b(0xC6, 0x05); a.d32(R + 0x40); a.b(0x01)
    # A callee that returns to a different address first; that code then returns to the call site.
    a.call("h2")
    a.label("h2ret"); a.b(0xC6, 0x05); a.d32(R + 0x45); a.b(0x03)
    # INT 21h reaches the runtime's interrupt service.
    a.b(0x66, 0xB8, 0x34, 0x12); a.b(0xCD, 0x21); a.b(0xA3); a.d32(R + 0x48)
    # Port input and output.
    a.b(0xE4, 0x60); a.b(0xA2); a.d32(R + 0x4C)
    a.b(0x66, 0xBA, 0xC8, 0x03); a.b(0xB0, 0x07); a.b(0xEE)
    # PUSHAD and POPAD, MOVSX and MOVZX.
    a.b(0xBE); a.d32(0x55); a.b(0x60); a.b(0xBE); a.d32(0); a.b(0x61); a.b(0x89, 0x35); a.d32(R + 0x50)
    a.b(0xB3, 0x80); a.b(0x0F, 0xBE, 0xC3); a.b(0xA3); a.d32(R + 0x54)
    a.b(0x0F, 0xB6, 0xC3); a.b(0xA3); a.d32(R + 0x58)
    # A far call through a 48-bit pointer (FF /3) returns with RETF.
    a.b(0xFF, 0x1D); a.d32(DATA_BASE + 0x500); a.b(0xA3); a.d32(R + 0x5C)
    a.b(0xC3)

    a.label("mul"); a.b(0x8B, 0x44, 0x24, 0x04); a.b(0x0F, 0xAF, 0x44, 0x24, 0x08); a.b(0xC2, 0x08, 0x00)
    a.label("g"); a.b(0xC7, 0x05); a.d32(R + 0x14); a.d32(0x1234); a.b(0xC3)
    a.label("h"); a.b(0x59); a.b(0xFF, 0xE1)
    a.label("farf"); a.b(0xB8); a.d32(0x99); a.b(0xCB)
    a.label("h2"); a.b(0x83, 0xC4, 0x04); a.b(0x68); a.code_address("else"); a.b(0xC3)
    a.label("else"); a.b(0xC6, 0x05); a.d32(R + 0x44); a.b(0x02); a.b(0x68); a.code_address("h2ret"); a.b(0xC3)
    code, code_fixups = a.finish()

    data = bytearray(0x1000)
    data[0x100:0x108] = b"ABCDEFGH"
    struct.pack_into("<3i", data, 0x300, 3, 4, 10)
    struct.pack_into("<H", data, 0x504, 0x10)
    table_fixups = b"".join(fixup32(0x400 + 4 * i, 1, a.labels[f"case{i}"]) for i in range(3)) + fixup32(0x500, 1, a.labels["farf"])
    objects = [(CODE_BASE, CODE_FLAGS, code, len(code)), (DATA_BASE, DATA_FLAGS, bytes(data), 0x1000)]
    return bound_program(objects, {1: code_fixups, 2: table_fixups})


def single_program(code: bytes):
    objects = [(CODE_BASE, CODE_FLAGS, code, len(code)), (DATA_BASE, DATA_FLAGS, bytes(0x1000), 0x1000)]
    return bound_program(objects, {})


def build_and_run(name: str, program: bytes, dump_start: int, dump_length: int, overrides=None, natives=False):
    folder = os.path.join(ROOT, "build", "tests", name)
    generated = os.path.join(folder, "generated")
    os.makedirs(generated, exist_ok=True)
    image = le.load(program)
    found = discover32.discover(image)
    emitter = emit32.write_program(image, found, f"{name}.exe", program, generated, 1, overrides)
    sources = [os.path.join(ROOT, "runtime", "core.cpp"), os.path.join(ROOT, "tests", "cpp", "emit_harness.cpp")]
    if natives:
        sources.append(os.path.join(ROOT, "tests", "cpp", "test_natives.cpp"))
    sources += [os.path.join(generated, f) for f in sorted(os.listdir(generated)) if f.endswith(".cpp")]
    exe = os.path.join(folder, "harness.exe")
    quoted = " ".join(f"'{s}'" for s in sources)
    command = (f". '{os.path.join(ROOT, 'tools', 'vsenv.ps1')}' *> $null; "
               f"cl.exe /nologo /std:c++20 /EHsc /MT /W3 /wd4102 /Od /DNOMINMAX /DWIN32_LEAN_AND_MEAN "
               f"'/I{ROOT}' '/I{generated}' '/Fo{folder}\\\\' '/Fe{exe}' {quoted}; exit $LASTEXITCODE")
    built = subprocess.run(["pwsh", "-NoProfile", "-Command", command], capture_output=True, text=True)
    if built.returncode != 0:
        raise AssertionError(f"compiling {name} failed:\n{built.stdout}\n{built.stderr}")
    ran = subprocess.run([exe, generated, f"{dump_start:x}", f"{dump_length:x}"], capture_output=True, text=True,
                         timeout=60)
    lines = ran.stdout.splitlines()
    return lines[0], lines[1], bytes.fromhex(lines[2]), emitter


@unittest.skipUnless(HAVE_COMPILER, "the Visual Studio C++ build tools are not installed")
class TranslatedProgramTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.outcome, cls.port, cls.memory, cls.emitter = build_and_run("main", main_program(), DATA_BASE, 0x300)

    def word(self, offset):
        return struct.unpack_from("<I", self.memory, offset)[0]

    def test_program_runs_to_its_final_return(self):
        self.assertEqual(self.outcome, "ok")

    def test_add_sets_carry_zero_overflow_and_sign(self):
        self.assertEqual(self.memory[0:4], b"\x01\x01\x01\x01")

    def test_compare_sets_signed_and_unsigned_less(self):
        self.assertEqual(self.memory[4:6], b"\x01\x01")

    def test_loop_instruction_repeats_until_ecx_is_zero(self):
        self.assertEqual(self.word(0x08), 55)

    def test_call_with_stack_arguments_returns_with_ret_n(self):
        self.assertEqual(self.word(0x0C), 12)

    def test_jump_table_reaches_the_selected_case(self):
        self.assertEqual(self.word(0x10), 300)

    def test_indirect_call_through_relocated_address(self):
        self.assertEqual(self.word(0x14), 0x1234)

    def test_rep_movsb_copies_the_bytes(self):
        self.assertEqual(self.memory[0x200:0x208], b"ABCDEFGH")

    def test_repne_movsd_copies_like_rep(self):
        self.assertEqual(self.memory[0x210:0x218], b"ABCDEFGH")

    def test_signed_division_gives_quotient_and_remainder(self):
        self.assertEqual((self.word(0x18), self.word(0x1C)), (14, 2))

    def test_x87_multiply_and_both_subtraction_orders(self):
        self.assertEqual(self.word(0x20), 12)
        self.assertEqual(struct.unpack_from("<i", self.memory, 0x24)[0], -7)
        self.assertEqual(self.word(0x28), 7)

    def test_x87_compare_reaches_carry_through_fnstsw_and_sahf(self):
        self.assertEqual(self.memory[0x2C], 1)

    def test_shift_left_carry_and_arithmetic_right_shift(self):
        self.assertEqual(self.memory[0x30], 1)
        self.assertEqual(self.word(0x34), 2)
        self.assertEqual(struct.unpack_from("<i", self.memory, 0x38)[0], -2)

    def test_writes_to_al_and_ah_keep_the_upper_half(self):
        self.assertEqual(self.word(0x3C), 0x1122EEFF)

    def test_return_through_pop_and_jump_continues_after_the_call(self):
        self.assertEqual(self.memory[0x40], 1)

    def test_return_to_another_address_runs_that_code_before_the_call_site(self):
        self.assertEqual(self.memory[0x44:0x46], b"\x02\x03")

    def test_software_interrupt_reaches_the_runtime(self):
        self.assertEqual(self.word(0x48), 0xABCD1234)

    def test_port_input_and_output_reach_the_runtime(self):
        self.assertEqual(self.memory[0x4C], 0x5A)
        self.assertEqual(self.port, "port 3c8 7")

    def test_pushad_and_popad_restore_registers(self):
        self.assertEqual(self.word(0x50), 0x55)

    def test_far_indirect_call_returns_with_retf(self):
        self.assertEqual(self.word(0x5C), 0x99)

    def test_movsx_and_movzx_extend_a_byte(self):
        self.assertEqual((self.word(0x54), self.word(0x58)), (0xFFFFFF80, 0x80))

    def test_jump_table_cases_stay_inside_the_function_chunk(self):
        cases = set(self.emitter.found.jump_tables[next(iter(self.emitter.found.jump_tables))])
        self.assertEqual(len(cases), 3)
        self.assertFalse(cases & set(self.emitter.starts))


@unittest.skipUnless(HAVE_COMPILER, "the Visual Studio C++ build tools are not installed")
class OverrideTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        a = Asm()
        a.b(0xB8); a.d32(21); a.call("f"); a.b(0xA3); a.d32(R + 0)
        a.label("site"); a.b(0xBB); a.d32(1)
        a.b(0x89, 0x1D); a.d32(R + 4)
        a.b(0xC3)
        a.label("f"); a.b(0xB8); a.d32(1); a.b(0xC3)
        code, fixups = a.finish()
        objects = [(CODE_BASE, CODE_FLAGS, code, len(code)), (DATA_BASE, DATA_FLAGS, bytes(0x1000), 0x1000)]
        overrides = {CODE_BASE + a.labels["f"]: "test_double", CODE_BASE + a.labels["site"]: "site:test_site"}
        cls.outcome, _, cls.memory, _ = build_and_run("overrides", bound_program(objects, {1: fixups}), DATA_BASE, 8,
                                                      overrides, natives=True)

    def test_overridden_function_runs_native_code_and_returns_to_its_caller(self):
        self.assertEqual(self.outcome, "ok")
        self.assertEqual(struct.unpack_from("<I", self.memory, 0)[0], 42)

    def test_site_override_replaces_only_that_instruction(self):
        self.assertEqual(struct.unpack_from("<I", self.memory, 4)[0], 0x77)


@unittest.skipUnless(HAVE_COMPILER, "the Visual Studio C++ build tools are not installed")
class StopTests(unittest.TestCase):
    def test_jump_to_untranslated_address_stops_with_that_address(self):
        code = bytes([0xB8]) + struct.pack("<I", DATA_BASE + 0x500) + bytes([0xFF, 0xE0])
        outcome, _, _, _ = build_and_run("untranslated", single_program(code), DATA_BASE, 4)
        self.assertTrue(outcome.startswith("fault The program reached address 00020500, which was not translated."), outcome)
        self.assertIn("Recently entered: 00010000 00020500.", outcome)

    def test_unsupported_instruction_stops_with_its_name(self):
        code = bytes([0x90, 0x27, 0xC3])
        outcome, _, _, _ = build_and_run("unsupported", single_program(code), DATA_BASE, 4)
        self.assertIn('"daa" at 00010001', outcome)


if __name__ == "__main__":
    unittest.main()
