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
    conflicts: list[tuple[int, int]] = field(default_factory=list)  # overlapping instructions that were kept
    undecodable: list[int] = field(default_factory=list)
    outside: list[tuple[int, int]] = field(default_factory=list)  # (from, target)
    unresolved_indirect: list[int] = field(default_factory=list)
    rejected: list[int] = field(default_factory=list)  # fixup targets in code that decode as data
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


# Instructions that Watcom code never contains. A trace from an unproven entry that decodes one of them is
# data that happens to be referenced by a fixup, not code.
IMPLAUSIBLE = {"arpl", "bound", "aaa", "aas", "aam", "aad", "daa", "das", "salc", "into", "int1", "insb", "insw",
               "insd", "lldt", "lidt", "lgdt", "ljmp", "lcall"}


def discover(image: le.LeImage, seeds: set[int] | None = None) -> Discovery:
    """Discovers code in two passes.

    The proven pass starts at the program entry and the given seeds and follows every control-flow edge.
    The second pass tries each fixup target inside code that the first pass did not reach. Such a target may
    be a function pointer or data stored in the code object, so its whole trace is accepted only when it
    decodes cleanly, contains no instruction that compiled code never uses and overlaps no accepted code.
    """
    result = Discovery()
    disassembler = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    disassembler.detail = True
    code = {obj.index: obj for obj in _code_objects(image) if obj.big}

    def in_code(address: int) -> le.LeObject | None:
        obj = image.object_at(address)
        return obj if obj is not None and obj.index in code else None

    owner: dict[int, int] = {}  # byte address -> start of the accepted instruction covering it

    def explore(starts, trace: Discovery, local_owner: dict[int, int], strict: bool) -> bool:
        pending = list(starts)
        while pending:
            address = pending.pop()
            recent: list[Instruction] = []
            while True:
                obj = in_code(address)
                if obj is None:
                    break
                if address in result.instructions or address in trace.instructions:
                    break
                taken = owner.get(address, local_owner.get(address))
                if taken is not None and strict:
                    return False
                # On the proven pass a branch into the middle of an instruction is real: compilers and library
                # code hide a second instruction inside the bytes of a first one to skip it cheaply. Both
                # readings are kept.
                overlapping = taken is not None
                window = bytes(obj.data[address - obj.base:address - obj.base + 16])
                decoded = next(disassembler.disasm(window, address, 1), None)
                if decoded is None:
                    if strict:
                        return False
                    trace.undecodable.append(address)
                    break
                clash = next((a for a in range(address, address + decoded.size)
                              if a in owner or a in local_owner), None)
                if clash is not None:
                    if strict:
                        return False
                    overlapping = True
                    trace.conflicts.append((address, owner.get(clash, local_owner.get(clash))))
                # Capstone reads a DS prefix on an indirect jump as Intel CET "notrack";
                # on a 386 the prefix only names the default segment, so drop it.
                mnemonic = decoded.mnemonic.removeprefix("notrack ").removeprefix("bnd ")
                if strict and (mnemonic.split()[-1] in IMPLAUSIBLE or mnemonic.startswith("fcmov")):
                    return False
                ins = Instruction(address, decoded.size, mnemonic, decoded.op_str)
                trace.instructions[address] = ins
                for a in range(address, address + decoded.size):
                    if not overlapping or (a not in owner and a not in local_owner):
                        local_owner[a] = address
                recent.append(ins)
                del recent[:-8]
                group_jump = decoded.group(capstone.CS_GRP_JUMP)
                group_call = decoded.group(capstone.CS_GRP_CALL)
                operand = decoded.operands[0] if decoded.operands else None

                if mnemonic == "int":
                    _record_interrupt(trace, decoded, recent)
                elif mnemonic in ("in", "out", "insb", "insw", "insd", "outsb", "outsw", "outsd"):
                    _record_port(trace, decoded, recent)

                if (group_jump or group_call) and operand is not None and operand.type == x86.X86_OP_IMM:
                    target = operand.imm & 0xFFFFFFFF
                    ins.target = target
                    if in_code(target):
                        pending.append(target)
                        if group_call:
                            trace.functions.add(target)
                    else:
                        if strict:
                            return False
                        trace.outside.append((address, target))
                elif (group_jump or group_call) and mnemonic not in ("ljmp", "lcall"):
                    table = _jump_table(image, decoded, mnemonic, in_code)
                    if table:
                        trace.jump_tables[address] = table
                        pending.extend(table)
                    else:
                        trace.unresolved_indirect.append(address)

                if mnemonic in TERMINATORS:
                    break
                address += decoded.size
        return True

    def accept(trace: Discovery, local_owner: dict[int, int]) -> None:
        result.instructions.update(trace.instructions)
        owner.update(local_owner)
        result.functions |= trace.functions
        result.conflicts += trace.conflicts
        result.undecodable += trace.undecodable
        result.outside += trace.outside
        result.unresolved_indirect += trace.unresolved_indirect
        result.jump_tables.update(trace.jump_tables)
        result.interrupts.update(trace.interrupts)
        result.ports.update(trace.ports)

    proven = [s for s in sorted({image.entry_linear} | (seeds or set())) if in_code(s)]
    result.entries.update(proven)
    result.functions.add(image.entry_linear)
    result.functions.update(proven)
    trace, local = Discovery(), {}
    explore(proven, trace, local, strict=False)
    accept(trace, local)

    for target in sorted(fixup_code_targets(image)):
        if not in_code(target) or target in result.instructions:
            continue
        if target in owner:
            result.rejected.append(target)
            continue
        trace, local = Discovery(), {}
        if explore([target], trace, local, strict=True):
            trace.functions.add(target)
            result.entries.add(target)
            accept(trace, local)
        else:
            result.rejected.append(target)
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
