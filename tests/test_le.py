"""Tests for the LE loader and 32-bit code discovery, built from small synthetic programs so no game files are needed."""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "recomp"))

import discover32  # noqa: E402
import le  # noqa: E402

PAGE = 0x1000
CODE_BASE = 0x10000
DATA_BASE = 0x20000


def fixup32(source_offset, target_object, target_offset):
    """An internal 32-bit offset fixup record with an 8-bit object number and a 16-bit target offset."""
    return struct.pack("<BBhBH", 0x07, 0x00, source_offset, target_object, target_offset)


def bound_program(objects, page_fixups, entry=(1, 0), stub_count=1, extender_padding=0x30):
    """Builds a file shaped like a DOS/4GW bound program.

    objects is a list of (base, flags, data, virtual_size); every object gets
    one page per started PAGE bytes of data. page_fixups maps a 1-based page
    number to its fixup records. stub_count controls how many MZ stubs point to
    an LE header, so the ambiguity check can be exercised.
    """
    pages = []
    table = b""
    for base, flags, data, size in objects:
        count = (len(data) + PAGE - 1) // PAGE
        table += struct.pack("<6I", size, base, flags, len(pages) + 1, count, 0)
        for i in range(count):
            pages.append(data[i * PAGE:(i + 1) * PAGE])
    header_size = 0xC4
    object_table = header_size
    page_map = object_table + len(table)
    page_map_bytes = b"".join(bytes([(n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF, 0])
                              for n in range(1, len(pages) + 1))
    fixup_page_table = page_map + len(page_map_bytes)
    records = b""
    offsets = []
    for page in range(1, len(pages) + 1):
        offsets.append(len(records))
        records += page_fixups.get(page, b"")
    offsets.append(len(records))
    fixup_page_bytes = struct.pack(f"<{len(offsets)}I", *offsets)
    fixup_records = fixup_page_table + len(fixup_page_bytes)
    loader_end = fixup_records + len(records)

    stub = bytearray(0x40)
    stub[0:2] = b"MZ"
    struct.pack_into("<I", stub, 0x3C, 0x40)
    # The extender part of a bound file comes first and contains an MZ stub of its own.
    extender = bytearray(b"MZ" + b"\0" * (extender_padding - 2))
    app_offset = len(extender)
    le_offset_in_app = 0x40
    data_pages = (le_offset_in_app + loader_end + 0xFF) // 0x100 * 0x100

    header = bytearray(header_size)
    header[0:4] = b"LE\x00\x00"
    struct.pack_into("<HH", header, 0x08, 2, 1)
    last = len(pages[-1]) if pages else PAGE
    fields = {
        0x14: len(pages), 0x18: entry[0], 0x1C: entry[1], 0x20: len(objects), 0x24: 0x100,
        0x28: PAGE, 0x2C: last, 0x40: object_table, 0x44: len(objects), 0x48: page_map,
        0x68: fixup_page_table, 0x6C: fixup_records, 0x80: data_pages, 0x94: len(objects),
    }
    for offset, value in fields.items():
        struct.pack_into("<I", header, offset, value)
    app = bytearray(stub) + header + table + page_map_bytes + fixup_page_bytes + records
    app = app.ljust(data_pages, b"\0")
    for i, page in enumerate(pages):
        app += page if i == len(pages) - 1 else page.ljust(PAGE, b"\0")
    result = bytes(extender) + bytes(app)
    for _ in range(stub_count - 1):
        result += bytes(stub) + bytes(header)
    return result


# Code object: mov ah,3Dh / int 21h / call function / jmp [eax*4+table] / two cases / function.
CODE = bytearray(0x30)
CODE[0x00:0x02] = b"\xB4\x3D"
CODE[0x02:0x04] = b"\xCD\x21"
CODE[0x04:0x09] = b"\xE8" + struct.pack("<i", 0x20 - 0x09)
CODE[0x09:0x10] = b"\xFF\x24\x85" + struct.pack("<I", 0)  # table address is filled by a fixup
CODE[0x10] = 0xC3
CODE[0x11] = 0xC3
CODE[0x20] = 0xC3
CODE = bytes(CODE)
DATA = bytes(16)
CODE_FLAGS = le.OBJ_READABLE | le.OBJ_EXECUTABLE | le.OBJ_BIG
DATA_FLAGS = le.OBJ_READABLE | le.OBJ_WRITABLE | le.OBJ_BIG


def sample_program(**options):
    objects = [(CODE_BASE, CODE_FLAGS, CODE, 0x30), (DATA_BASE, DATA_FLAGS, DATA, 0x2000)]
    fixups = {
        1: fixup32(0x0C, 2, 0x0),
        2: fixup32(0x0, 1, 0x10) + fixup32(0x4, 1, 0x11),
    }
    return bound_program(objects, fixups, **options)


class LoaderTests(unittest.TestCase):
    def test_bound_program_objects_load_at_their_base_addresses(self):
        image = le.load(sample_program())
        self.assertEqual(image.object(1).base, CODE_BASE)
        self.assertEqual(image.object(2).base, DATA_BASE)
        self.assertEqual(image.read(CODE_BASE, 2), b"\xB4\x3D")
        self.assertEqual(image.entry_linear, CODE_BASE)

    def test_offsets_count_from_the_application_stub_not_from_the_extender(self):
        image = le.load(sample_program(extender_padding=0x230))
        self.assertEqual(image.app_offset, 0x230)
        self.assertEqual(image.read(CODE_BASE + 0x20, 1), b"\xC3")

    def test_internal_fixups_write_linear_addresses(self):
        image = le.load(sample_program())
        self.assertEqual(struct.unpack("<I", image.read(CODE_BASE + 0x0C, 4))[0], DATA_BASE)
        self.assertEqual(struct.unpack("<2I", image.read(DATA_BASE, 8)), (CODE_BASE + 0x10, CODE_BASE + 0x11))

    def test_space_beyond_the_stored_pages_reads_as_zero(self):
        image = le.load(sample_program())
        self.assertEqual(image.read(DATA_BASE + 0x1FF0, 16), bytes(16))

    def test_two_stubs_pointing_to_le_headers_are_rejected_as_ambiguous(self):
        with self.assertRaisesRegex(le.LeError, "several MZ stubs"):
            le.load(sample_program(stub_count=2))

    def test_file_without_le_header_is_rejected(self):
        with self.assertRaisesRegex(le.LeError, "no MZ stub"):
            le.load(b"MZ" + bytes(0x100))

    def test_truncated_data_pages_are_rejected(self):
        data = sample_program()
        with self.assertRaisesRegex(le.LeError, "past the end"):
            le.load(data[:-8])

    def test_imported_fixup_is_rejected_instead_of_ignored(self):
        objects = [(CODE_BASE, CODE_FLAGS, CODE, 0x30)]
        imported = struct.pack("<BBhBH", 0x07, 0x01, 0x0C, 1, 1)
        with self.assertRaisesRegex(le.LeError, "imported ordinal"):
            le.load(bound_program(objects, {1: imported}))

    def test_fixup_that_leaves_its_object_is_rejected(self):
        objects = [(CODE_BASE, CODE_FLAGS, CODE, 0x30)]
        with self.assertRaisesRegex(le.LeError, "leaves the object"):
            le.load(bound_program(objects, {1: fixup32(0x2E, 1, 0)}))


class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.image = le.load(sample_program())
        self.found = discover32.discover(self.image)

    def test_direct_call_target_is_discovered_as_a_function(self):
        self.assertIn(CODE_BASE + 0x20, self.found.functions)
        self.assertIn(CODE_BASE + 0x20, self.found.instructions)

    def test_jump_table_entries_are_read_through_their_fixups(self):
        self.assertEqual(self.found.jump_tables[CODE_BASE + 0x09], [CODE_BASE + 0x10, CODE_BASE + 0x11])

    def test_dos_call_is_recorded_with_its_function_number(self):
        self.assertEqual(self.found.interrupts[(0x21, 0x3D00)], 1)

    def test_bytes_never_reached_are_not_counted_as_code(self):
        self.assertNotIn(CODE_BASE + 0x12, self.found.instructions)
        self.assertEqual(self.found.covered_bytes(self.image.object(1)), 0x10 + 2 + 1)

    def test_overlapping_instruction_is_reported_as_a_conflict(self):
        found = discover32.discover(self.image, seeds={CODE_BASE + 0x01})
        self.assertTrue(found.conflicts)


if __name__ == "__main__":
    unittest.main()
