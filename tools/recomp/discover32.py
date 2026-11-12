"""Code discovery for 32-bit flat-model programs loaded by le.py.

Discovery follows control flow by recursive descent from proven entry points:
the program entry, every address that an internal fixup places inside an
executable object, and the targets of jump tables that can be read off a
`jmp [reg*4+table]` instruction. Bytes are only counted as code once an
instruction decoded from a proven entry covers them. Two instructions that
overlap are reported as a conflict instead of being resolved silently.

The pass also records what the program asks of DOS, the extender and the PC
hardware: software interrupts with the function number loaded before them, and
port input and output with the port number where it can be seen.
"""

from __future__ import annotations

import collections
import struct
from dataclasses import dataclass, field

import capstone
from capstone import x86

import le

TERMINATORS = {"ret", "retf", "iret", "iretd", "hlt", "jmp", "ljmp", "ud2"}


@dataclass
class Instruction:
    address: int
    size: int
    mnemonic: str
    op_str: str
    target: int | None = None  # direct branch or call target


@dataclass
class Discovery:
    instructions: dict[int, Instruction] = field(default_factory=dict)
    entries: set[int] = field(default_factory=set)
    functions: set[int] = field(default_factory=set)
    conflicts: list[tuple[int, int]] = field(default_factory=list)
    undecodable: list[int] = field(default_factory=list)
    outside: list[tuple[int, int]] = field(default_factory=list)  # (from, target)
    unresolved_indirect: list[int] = field(default_factory=list)
    jump_tables: dict[int, list[int]] = field(default_factory=dict)
    interrupts: collections.Counter = field(default_factory=collections.Counter)
    ports: collections.Counter = field(default_factory=collections.Counter)

    def covered_bytes(self, obj: le.LeObject) -> int:
        covered = bytearray(obj.virtual_size)
        for ins in self.instructions.values():
            if obj.base <= ins.address < obj.end:
                start = ins.address - obj.base
                covered[start:start + ins.size] = b"\x01" * ins.size
        return sum(covered)


def _code_objects(image: le.LeImage) -> list[le.LeObject]:
    return [obj for obj in image.objects if obj.executable]


def fixup_code_targets(image: le.LeImage) -> set[int]:
    """Linear addresses inside 32-bit code that some fixup points to."""
    targets = set()
    for fixup in image.fixups:
        target = image.object(fixup.target_object)
        if target.executable and target.big and fixup.source_type == le.SRC_OFFSET32:
            targets.add(target.base + fixup.target_offset)
    return targets


def discover(image: le.LeImage, seeds: set[int] | None = None) -> Discovery:
    result = Discovery()
    disassembler = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    disassembler.detail = True
    code = {obj.index: obj for obj in _code_objects(image) if obj.big}

    def in_code(address: int) -> le.LeObject | None:
        obj = image.object_at(address)
        return obj if obj is not None and obj.index in code else None

    owner: dict[int, int] = {}  # byte address -> start of the instruction covering it
    pending = []
    starts = {image.entry_linear} | fixup_code_targets(image) | (seeds or set())
    for start in sorted(starts):
        if in_code(start):
            result.entries.add(start)
            pending.append(start)
    result.functions.add(image.entry_linear)

    while pending:
        address = pending.pop()
        recent: list[Instruction] = []
        while True:
            obj = in_code(address)
            if obj is None:
                break
            if address in result.instructions:
                break
            if address in owner:
                result.conflicts.append((address, owner[address]))
                break
            window = bytes(obj.data[address - obj.base:address - obj.base + 16])
            decoded = next(disassembler.disasm(window, address, 1), None)
            if decoded is None:
                result.undecodable.append(address)
                break
            clash = next((a for a in range(address, address + decoded.size) if a in owner), None)
            if clash is not None:
                result.conflicts.append((address, owner[clash]))
                break
            # Capstone reads a DS prefix on an indirect jump as Intel CET "notrack";
            # on a 386 the prefix only names the default segment, so drop it.
            mnemonic = decoded.mnemonic.removeprefix("notrack ").removeprefix("bnd ")
            ins = Instruction(address, decoded.size, mnemonic, decoded.op_str)
            result.instructions[address] = ins
            for a in range(address, address + decoded.size):
                owner[a] = address
            recent.append(ins)
            del recent[:-8]
            group_jump = decoded.group(capstone.CS_GRP_JUMP)
            group_call = decoded.group(capstone.CS_GRP_CALL)
            operand = decoded.operands[0] if decoded.operands else None

            if mnemonic == "int":
                _record_interrupt(result, decoded, recent)
            elif mnemonic in ("in", "out", "insb", "insw", "insd", "outsb", "outsw", "outsd"):
                _record_port(result, decoded, recent)

            if (group_jump or group_call) and operand is not None and operand.type == x86.X86_OP_IMM:
                target = operand.imm & 0xFFFFFFFF
                ins.target = target
                if in_code(target):
                    if target not in result.instructions:
                        pending.append(target)
                    if group_call:
                        result.functions.add(target)
                else:
                    result.outside.append((address, target))
            elif (group_jump or group_call) and mnemonic not in ("ljmp", "lcall"):
                table = _jump_table(image, decoded, mnemonic, in_code)
                if table:
                    result.jump_tables[address] = table
                    pending.extend(t for t in table if t not in result.instructions)
                else:
                    result.unresolved_indirect.append(address)

            if mnemonic in TERMINATORS:
                break
            address += decoded.size
    return result


def _jump_table(image: le.LeImage, decoded, mnemonic: str, in_code) -> list[int]:
    """Targets of `jmp [index*4 + table]` when every entry is relocated into code."""
    if mnemonic != "jmp" or not decoded.operands:
        return []
    operand = decoded.operands[0]
    if operand.type != x86.X86_OP_MEM:
        return []
    memory = operand.mem
    if memory.base != 0 or memory.index == 0 or memory.scale != 4:
        return []
    table = memory.disp & 0xFFFFFFFF
    relocated = {image.object(f.object_index).base + f.offset
                 for f in image.fixups if f.source_type == le.SRC_OFFSET32}
    targets = []
    entry = table
    while entry in relocated:
        try:
            value = struct.unpack("<I", image.read(entry, 4))[0]
        except le.LeError:
            break
        if not in_code(value):
            break
        targets.append(value)
        entry += 4
    return targets


def _last_immediate(recent, register_names):
    for ins in reversed(recent[:-1]):
        if ins.mnemonic == "mov":
            parts = [p.strip() for p in ins.op_str.split(",")]
            if len(parts) == 2 and parts[0] in register_names:
                try:
                    return parts[0], int(parts[1], 0)
                except ValueError:
                    return None
        written = ins.op_str.split(",")[0].strip()
        if written in register_names:
            return None
    return None


def _record_interrupt(result: Discovery, decoded, recent) -> None:
    number = decoded.operands[0].imm & 0xFF
    function = None
    if number in (0x21, 0x10, 0x16, 0x2F, 0x33):
        loaded = _last_immediate(recent, ("ah", "ax", "eax"))
        if loaded:
            name, value = loaded
            function = (value << 8) if name == "ah" else value & 0xFFFF
            if number == 0x21:
                function &= 0xFF00
    elif number == 0x31:
        loaded = _last_immediate(recent, ("ax", "eax"))
        if loaded:
            function = loaded[1] & 0xFFFF
    result.interrupts[(number, function)] += 1


def _record_port(result: Discovery, decoded, recent) -> None:
    port = None
    for operand in decoded.operands:
        if operand.type == x86.X86_OP_IMM:
            port = operand.imm & 0xFF
    if port is None:
        loaded = _last_immediate(recent, ("dx", "edx"))
        if loaded:
            port = loaded[1] & 0xFFFF
    result.ports[(decoded.mnemonic, port)] += 1
