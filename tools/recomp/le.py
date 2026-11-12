"""Loader for Linear Executable (LE) programs bound to a DOS extender.

Watcom programs for DOS/4GW are stored as a chain: the extender's own MZ stub and
data come first, and the application follows as an MZ stub whose e_lfanew field
points to an LE header. In such a bound file every offset that the LE format
calls "relative to the file" is relative to the start of the application's MZ
stub, not to the start of the bound file.

The loader places every object at its preferred base address and applies the
internal fixups, which is what DOS/4GW does in its flat memory model. It refuses
any input whose structures do not add up instead of guessing.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field


class LeError(ValueError):
    """Raised when a file is not a well-formed LE program that the loader supports."""


OBJ_READABLE = 0x0001
OBJ_WRITABLE = 0x0002
OBJ_EXECUTABLE = 0x0004
OBJ_BIG = 0x2000

PAGE_LEGAL = 0
PAGE_ITERATED = 1
PAGE_INVALID = 2
PAGE_ZERO = 3

# Fixup source types (low nibble of the source byte).
SRC_BYTE = 0x00
SRC_SELECTOR16 = 0x02
SRC_POINTER16_16 = 0x03
SRC_OFFSET16 = 0x05
SRC_POINTER16_32 = 0x06
SRC_OFFSET32 = 0x07
SRC_RELATIVE32 = 0x08

SOURCE_SIZES = {
    SRC_BYTE: 1,
    SRC_SELECTOR16: 2,
    SRC_POINTER16_16: 4,
    SRC_OFFSET16: 2,
    SRC_POINTER16_32: 6,
    SRC_OFFSET32: 4,
    SRC_RELATIVE32: 4,
}


@dataclass
class LeObject:
    index: int  # 1-based, as in the LE format
    virtual_size: int
    base: int
    flags: int
    first_page: int  # 1-based index into the object page map
    page_count: int
    data: bytearray = field(default_factory=bytearray)

    @property
    def executable(self) -> bool:
        return bool(self.flags & OBJ_EXECUTABLE)

    @property
    def big(self) -> bool:
        return bool(self.flags & OBJ_BIG)

    @property
    def end(self) -> int:
        return self.base + self.virtual_size


@dataclass
class Fixup:
    object_index: int  # object that contains the patched bytes
    offset: int  # offset of the patched bytes inside that object
    source_type: int
    target_object: int
    target_offset: int


@dataclass
class LeImage:
    app_offset: int  # file offset of the application's MZ stub
    header_offset: int  # file offset of the LE header
    objects: list[LeObject]
    entry_object: int
    entry_offset: int
    stack_object: int
    stack_offset: int
    auto_data_object: int
    fixups: list[Fixup]

    def object(self, index: int) -> LeObject:
        if not 1 <= index <= len(self.objects):
            raise LeError(f"object {index} does not exist")
        return self.objects[index - 1]

    @property
    def entry_linear(self) -> int:
        return self.object(self.entry_object).base + self.entry_offset

    @property
    def stack_linear(self) -> int:
        return self.object(self.stack_object).base + self.stack_offset

    def object_at(self, linear: int) -> LeObject | None:
        for obj in self.objects:
            if obj.base <= linear < obj.end:
                return obj
        return None

    def read(self, linear: int, size: int) -> bytes:
        obj = self.object_at(linear)
        if obj is None or linear + size > obj.end:
            raise LeError(f"read of {size} bytes at {linear:#x} leaves the loaded objects")
        start = linear - obj.base
        return bytes(obj.data[start:start + size])


def find_application(data: bytes) -> tuple[int, int]:
    """Return (application MZ offset, LE header offset) of a bound program.

    Exactly one MZ header in the file may point to an LE header; anything else is
    rejected, because picking one of several candidates would be a guess.
    """
    candidates = []
    position = data.find(b"MZ")
    while position >= 0:
        if position + 0x40 <= len(data):
            lfanew = struct.unpack_from("<I", data, position + 0x3C)[0]
            header = position + lfanew
            if 0 < lfanew and header + 0xC4 <= len(data) and data[header:header + 4] == b"LE\x00\x00":
                candidates.append((position, header))
        position = data.find(b"MZ", position + 1)
    if not candidates:
        raise LeError("no MZ stub points to an LE header")
    if len(candidates) > 1:
        listed = ", ".join(f"{mz:#x}" for mz, _ in candidates)
        raise LeError(f"several MZ stubs point to LE headers ({listed})")
    return candidates[0]


def _u8(data: bytes, offset: int) -> int:
    if offset + 1 > len(data):
        raise LeError(f"fixup data ends early at {offset:#x}")
    return data[offset]


def _u16(data: bytes, offset: int) -> int:
    if offset + 2 > len(data):
        raise LeError(f"fixup data ends early at {offset:#x}")
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: bytes, offset: int) -> int:
    if offset + 4 > len(data):
        raise LeError(f"fixup data ends early at {offset:#x}")
    return struct.unpack_from("<I", data, offset)[0]


def load(data: bytes, shift: int = 0) -> LeImage:
    """Loads a bound program. shift moves every object up by that many bytes, as an extender that
    loads the program elsewhere would; the fixups then produce the moved addresses."""
    app, le = find_application(data)

    def field32(offset: int) -> int:
        return struct.unpack_from("<I", data, le + offset)[0]

    if data[le + 2] != 0 or data[le + 3] != 0:
        raise LeError("only little-endian byte and word order is supported")
    if field32(0x04) != 0:
        raise LeError(f"unsupported LE format level {field32(0x04)}")

    page_count = field32(0x14)
    entry_object, entry_offset = field32(0x18), field32(0x1C)
    stack_object, stack_offset = field32(0x20), field32(0x24)
    page_size = field32(0x28)
    last_page_size = field32(0x2C)
    object_table = le + field32(0x40)
    object_count = field32(0x44)
    page_map = le + field32(0x48)
    fixup_page_table = le + field32(0x68)
    fixup_records = le + field32(0x6C)
    import_module_count = field32(0x74)
    data_pages = app + field32(0x80)
    auto_data_object = field32(0x94)

    if page_size == 0 or page_size & (page_size - 1):
        raise LeError(f"page size {page_size:#x} is not a power of two")
    if not 0 < last_page_size <= page_size:
        raise LeError(f"last page size {last_page_size:#x} does not fit the page size")
    if object_count == 0:
        raise LeError("the program has no objects")
    if page_count and data_pages + (page_count - 1) * page_size + last_page_size > len(data):
        raise LeError("the data pages run past the end of the file")

    page_flags: list[int] = []
    page_numbers: list[int] = []
    for i in range(page_count):
        entry = data[page_map + 4 * i:page_map + 4 * i + 4]
        if len(entry) != 4:
            raise LeError("the object page map runs past the end of the file")
        page_numbers.append((entry[0] << 16) | (entry[1] << 8) | entry[2])
        page_flags.append(entry[3])

    objects: list[LeObject] = []
    for i in range(object_count):
        raw = data[object_table + 24 * i:object_table + 24 * i + 24]
        if len(raw) != 24:
            raise LeError("the object table runs past the end of the file")
        size, base, flags, first_page, count, _ = struct.unpack("<6I", raw)
        base += shift
        if count and (first_page < 1 or first_page + count - 1 > page_count):
            raise LeError(f"object {i + 1} refers to pages outside the page map")
        # Bytes beyond the stored pages (uninitialised data and the stack) stay zero.
        obj =LeObject(i + 1, size, base, flags, first_page, count, bytearray(size))
        for p in range(count):
            page_index = first_page + p  # 1-based
            flag = page_flags[page_index - 1]
            if flag == PAGE_ZERO:
                continue
            if flag != PAGE_LEGAL:
                raise LeError(f"page {page_index} has unsupported type {flag}")
            number = page_numbers[page_index - 1]
            if not 1 <= number <= page_count:
                raise LeError(f"page {page_index} names data page {number}, which does not exist")
            stored = last_page_size if number == page_count else page_size
            start = data_pages + (number - 1) * page_size
            chunk = data[start:start + stored]
            target = p * page_size
            if target >= size:
                raise LeError(f"object {i + 1} stores more pages than its size allows")
            chunk = chunk[:size - target]
            obj.data[target:target + len(chunk)] = chunk
        objects.append(obj)

    for a in objects:
        for b in objects:
            if a.index < b.index and a.base < b.end and b.base < a.end:
                raise LeError(f"objects {a.index} and {b.index} overlap")

    if not 1 <= entry_object <= object_count or not objects[entry_object - 1].executable:
        raise LeError("the entry point is not in an executable object")

    fixups = _read_fixups(data, objects, page_count, page_size, fixup_page_table,
                          fixup_records, import_module_count)
    image = LeImage(app, le, objects, entry_object, entry_offset, stack_object, stack_offset,
                    auto_data_object, fixups)
    _apply_fixups(image)
    return image


def _page_owner(objects: list[LeObject], page_index: int) -> tuple[LeObject, int]:
    for obj in objects:
        if obj.first_page <= page_index < obj.first_page + obj.page_count:
            return obj, (page_index - obj.first_page)
    raise LeError(f"page {page_index} has fixups but belongs to no object")


def _read_fixups(data: bytes, objects: list[LeObject], page_count: int, page_size: int,
                 page_table: int, records: int, import_module_count: int) -> list[Fixup]:
    offsets = [_u32(data, page_table + 4 * i) for i in range(page_count + 1)]
    fixups: list[Fixup] = []
    for page in range(page_count):
        position = records + offsets[page]
        end = records + offsets[page + 1]
        if end < position:
            raise LeError(f"the fixup page table goes backwards at page {page + 1}")
        if position == end:
            continue
        obj, page_in_object = _page_owner(objects, page + 1)
        page_base = page_in_object * page_size
        while position < end:
            source = _u8(data, position)
            flags = _u8(data, position + 1)
            position += 2
            source_type = source & 0x0F
            if source & 0x20:
                raise LeError(f"alias fixups are not supported (page {page + 1})")
            if source_type not in SOURCE_SIZES:
                raise LeError(f"unknown fixup source type {source_type:#x} on page {page + 1}")
            target_kind = flags & 0x03
            if target_kind != 0:
                kind = {1: "imported ordinal", 2: "imported name", 3: "entry table"}[target_kind]
                raise LeError(f"{kind} fixups are not supported (page {page + 1}); "
                              f"the program declares {import_module_count} import modules")
            if flags & 0x04:
                raise LeError(f"additive fixups are not supported (page {page + 1})")
            source_list = bool(source & 0x10)
            if source_list:
                count = _u8(data, position)
                position += 1
                source_offsets: list[int] = []
            else:
                source_offsets = [struct.unpack("<h", bytes([_u8(data, position), _u8(data, position + 1)]))[0]]
                position += 2
            if flags & 0x40:
                target_object = _u16(data, position)
                position += 2
            else:
                target_object = _u8(data, position)
                position += 1
            if not 1 <= target_object <= len(objects):
                raise LeError(f"fixup on page {page + 1} targets object {target_object}, which does not exist")
            if source_type == SRC_SELECTOR16:
                target_offset = 0
            elif flags & 0x10:
                target_offset = _u32(data, position)
                position += 4
            else:
                target_offset = _u16(data, position)
                position += 2
            if source_list:
                for _ in range(count):
                    source_offsets.append(struct.unpack("<h", bytes([_u8(data, position), _u8(data, position + 1)]))[0])
                    position += 2
            for source_offset in source_offsets:
                fixups.append(Fixup(obj.index, page_base + source_offset, source_type,
                                    target_object, target_offset))
        if position != end:
            raise LeError(f"the fixups of page {page + 1} run past their table entry")
    return fixups


def _apply_fixups(image: LeImage) -> None:
    for fixup in image.fixups:
        obj = image.object(fixup.object_index)
        size = SOURCE_SIZES[fixup.source_type]
        # A fixup that straddles a page boundary is recorded on both pages; the
        # second record has a negative offset. Both write the same value, so the
        # duplicate is harmless, but the bytes must still lie inside the object.
        if fixup.offset < 0 or fixup.offset + size > obj.virtual_size:
            raise LeError(f"fixup at object {obj.index}+{fixup.offset:#x} leaves the object")
        target = image.object(fixup.target_object)
        linear = target.base + fixup.target_offset
        if fixup.source_type == SRC_OFFSET32:
            struct.pack_into("<I", obj.data, fixup.offset, linear & 0xFFFFFFFF)
        elif fixup.source_type == SRC_RELATIVE32:
            source_linear = obj.base + fixup.offset + 4
            struct.pack_into("<I", obj.data, fixup.offset, (linear - source_linear) & 0xFFFFFFFF)
        elif fixup.source_type == SRC_OFFSET16:
            struct.pack_into("<H", obj.data, fixup.offset, fixup.target_offset & 0xFFFF)
        elif fixup.source_type == SRC_SELECTOR16:
            # Selectors are only known at run time. The runtime decides them, so the
            # loader records the fixup and leaves a marker naming the target object.
            struct.pack_into("<H", obj.data, fixup.offset, fixup.target_object)
        else:
            raise LeError(f"fixup source type {fixup.source_type:#x} is not supported yet "
                          f"(object {obj.index}+{fixup.offset:#x})")
