"""Translates the discovered 32-bit code of an LE program into C++ for the native runtime.

Usage: python tools/recomp/emit32.py <program> <output folder> [--seeds <file>] [--files <count>]

The code is split into chunks: every function that discovery found starts a chunk, and a chunk runs up to
the next one. Each chunk becomes one C++ function with a switch over all of its instruction addresses, so it
can be entered wherever a return or a computed jump lands. Direct jumps inside a chunk are gotos. Calls push
the original return address on the guest stack and call the target chunk directly; when the callee does not
come back to that address, the dispatcher keeps running until it does. Leaving a chunk any other way sets EIP
and returns to the dispatcher. Reaching an address that was not translated stops the program with a message
that names it, and so does an instruction the translator does not support.

The output folder receives the chunk sources, a header with their declarations, the program table and
image.bin with the object contents after fixups. All of it is derived from the original program, so it stays
on the machine of the user who owns that program.
"""

from __future__ import annotations

import argparse
import binascii
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import capstone  # noqa: E402
from capstone import x86  # noqa: E402

import discover32  # noqa: E402
import le  # noqa: E402

REG32 = {"eax": "a", "ebx": "b", "ecx": "c", "edx": "d", "esi": "si", "edi": "di", "ebp": "bp", "esp": "sp"}
REG16 = {"ax": "a", "bx": "b", "cx": "c", "dx": "d", "si": "si", "di": "di", "bp": "bp", "sp": "sp"}
REG8 = {"al": ("a", "l"), "ah": ("a", "h"), "bl": ("b", "l"), "bh": ("b", "h"),
        "cl": ("c", "l"), "ch": ("c", "h"), "dl": ("d", "l"), "dh": ("d", "h")}
SREG = {"es": "ES", "cs": "CS", "ss": "SS", "ds": "DS", "fs": "FS", "gs": "GS"}
SEG_PREFIX = {0x26: "ES", 0x2E: "CS", 0x36: "SS", 0x3E: "DS", 0x64: "FS", 0x65: "GS"}
PREFIX_BYTES = {0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0x9B, 0xF0, 0xF2, 0xF3}

CONDITIONS = {
    "o": "c.of", "no": "!c.of", "b": "c.cf", "nae": "c.cf", "c": "c.cf", "ae": "!c.cf", "nb": "!c.cf", "nc": "!c.cf",
    "e": "c.zf", "z": "c.zf", "ne": "!c.zf", "nz": "!c.zf", "be": "(c.cf || c.zf)", "na": "(c.cf || c.zf)",
    "a": "(!c.cf && !c.zf)", "nbe": "(!c.cf && !c.zf)", "s": "c.sf", "ns": "!c.sf", "p": "c.pf()", "pe": "c.pf()",
    "np": "!c.pf()", "po": "!c.pf()", "l": "(c.sf != c.of)", "nge": "(c.sf != c.of)", "ge": "(c.sf == c.of)",
    "nl": "(c.sf == c.of)", "le": "(c.zf || c.sf != c.of)", "ng": "(c.zf || c.sf != c.of)",
    "g": "(!c.zf && c.sf == c.of)", "nle": "(!c.zf && c.sf == c.of)",
}
ARITH = {"add": ("add", ""), "adc": ("add", "c.cf"), "sub": ("sub", ""), "sbb": ("sub", "c.cf"), "cmp": ("sub", "")}
LOGIC = {"and": "&", "or": "|", "xor": "^", "test": "&"}
SHIFTS = {"shl": "shl", "sal": "shl", "shr": "shr", "sar": "sar", "rol": "rol", "ror": "ror", "rcl": "rcl", "rcr": "rcr"}
STRING_OPS = {"movs", "stos", "lods", "scas", "cmps", "ins", "outs"}
UNSUPPORTED = {"arpl", "sldt", "sidt", "sgdt", "lidt", "lgdt", "lldt", "ltr", "str", "bound", "aaa", "aas", "aam",
               "aad", "daa", "das", "salc", "ljmp", "lcall", "invd", "wbinvd", "clts", "rdtsc", "cpuid", "verr", "verw"}


class EmitError(Exception):
    """An instruction that cannot be translated. It becomes a run-time stop, not a build failure."""


def hexu(value: int) -> str:
    return f"0x{value & 0xFFFFFFFF:x}u"


class Emitter:
    def __init__(self, image: le.LeImage, found: discover32.Discovery):
        self.image = image
        self.found = found
        self.disassembler = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.disassembler.detail = True
        self.selectors = {}
        for fixup in image.fixups:
            if fixup.source_type == le.SRC_SELECTOR16:
                self.selectors[image.object(fixup.object_index).base + fixup.offset] = fixup.target_object
        self.unsupported: list[tuple[int, str]] = []
        self._build_chunks()

    # Chunks --------------------------------------------------------------------------------------------

    def _build_chunks(self) -> None:
        addresses = sorted(self.found.instructions)
        table_targets = {t for targets in self.found.jump_tables.values() for t in targets}
        starts = set(self.found.functions)
        starts |= (discover32.fixup_code_targets(self.image) - table_targets)
        starts &= set(addresses)
        if addresses:
            starts.add(addresses[0])
        self.starts = sorted(starts)
        self.chunk_of: dict[int, int] = {}
        self.chunks: list[list[int]] = [[] for _ in self.starts]
        index = -1
        for address in addresses:
            while index + 1 < len(self.starts) and self.starts[index + 1] <= address:
                index += 1
            self.chunks[index].append(address)
            self.chunk_of[address] = index
        self.start_set = set(self.starts)

    @staticmethod
    def label(address: int) -> str:
        return f"L_{address:08x}"

    @staticmethod
    def chunk_name(address: int) -> str:
        return f"ch_{address:08x}"

    # Operands ------------------------------------------------------------------------------------------

    def reg(self, name: str, bits_hint: int | None = None):
        if name in REG32:
            r = f"c.{REG32[name]}.e"
            return r, (lambda v, r=r: f"{r} = (uint32_t)({v});"), 32
        if name in REG16:
            r = f"c.{REG16[name]}.x"
            return r, (lambda v, r=r: f"{r} = (uint16_t)({v});"), 16
        if name in REG8:
            reg, part = REG8[name]
            r = f"c.{reg}.{part}"
            return r, (lambda v, r=r: f"{r} = (uint8_t)({v});"), 8
        if name in SREG:
            s = SREG[name]
            return f"c.sel[{s}]", (lambda v, s=s: f"m.load_seg({s}, (uint16_t)({v}));"), 16
        raise EmitError(f"register {name} is not supported")

    def offset(self, insn, op) -> str:
        memory = op.mem
        terms = []
        if memory.base:
            name = insn.reg_name(memory.base)
            if name not in REG32:
                raise EmitError("16-bit addressing is not supported")
            terms.append(f"c.{REG32[name]}.e")
        if memory.index:
            name = insn.reg_name(memory.index)
            if name not in REG32:
                raise EmitError("16-bit addressing is not supported")
            index = f"c.{REG32[name]}.e"
            terms.append(index if memory.scale == 1 else f"{index} * {memory.scale}u")
        if memory.disp or not terms:
            terms.append(hexu(memory.disp))
        return "(uint32_t)(" + " + ".join(terms) + ")" if len(terms) > 1 else terms[0]

    def segment(self, insn, op) -> str:
        memory = op.mem
        if memory.segment:
            return SREG[insn.reg_name(memory.segment)]
        if memory.base and insn.reg_name(memory.base) in ("ebp", "esp"):
            return "SS"
        return "DS"

    def address(self, insn, op) -> str:
        return f"(uint32_t)(c.base[{self.segment(insn, op)}] + {self.offset(insn, op)})"

    def operand(self, insn, op, temps: list[str], width: int | None = None):
        """Returns (read expression, write function or None, size in bits)."""
        if op.type == x86.X86_OP_REG:
            return self.reg(insn.reg_name(op.reg))
        if op.type == x86.X86_OP_IMM:
            bits = (width or op.size * 8) or 32
            mask = (1 << bits) - 1
            # A selector fixup on the last word of the instruction makes this immediate a selector.
            end = insn.address + insn.size
            if bits == 16 and end - 2 in self.selectors:
                return f"m.object_selector({self.selectors[end - 2]})", None, 16
            return hexu(op.imm & mask), None, bits
        if op.type == x86.X86_OP_MEM:
            n = len(temps)
            temps.append(f"const uint32_t a{n} = {self.address(insn, op)};")
            bits = op.size * 8
            if bits not in (8, 16, 32):
                raise EmitError(f"a {bits}-bit memory operand is not supported here")
            return f"m.rd{bits}(a{n})", (lambda v, b=bits, n=n: f"m.wr{b}(a{n}, (uint{b}_t)({v}));"), bits
        raise EmitError("unknown operand type")

    def check_fixups(self, insn) -> None:
        end = insn.address + insn.size
        for at in range(insn.address, end):
            if at in self.selectors and at != end - 2:
                raise EmitError("a selector fixup lies inside the instruction")

    # Control flow --------------------------------------------------------------------------------------

    def jump(self, here: int, target: int) -> str:
        if self.chunk_of.get(target) == self.chunk_of[here]:
            if target <= here:
                return f"if (--m.budget < 0) {{ c.eip = {hexu(target)}; m.poll(); }} goto {self.label(target)};"
            return f"goto {self.label(target)};"
        return f"c.eip = {hexu(target)}; return;"

    def call(self, target_expr: str, ret: int, direct: int | None) -> str:
        head = f"const uint32_t t = {target_expr}; " if direct is None else ""
        push = f"const uint32_t sp0 = c.sp.e; m.push32({hexu(ret)}); "
        if direct is not None and direct in self.start_set:
            return (f"{{ {push}c.eip = {hexu(direct)}; {self.chunk_name(direct)}(m); "
                    f"m.after_call({hexu(ret)}, sp0); }}")
        target = hexu(direct) if direct is not None else "t"
        return f"{{ {head}{push}c.eip = {target}; m.run_until({hexu(ret)}, sp0); }}"

    # Instructions --------------------------------------------------------------------------------------

    def translate(self, insn) -> tuple[str, bool]:
        """Returns the C++ statements for one instruction and whether control never falls through."""
        self.check_fixups(insn)
        temps: list[str] = []
        body, terminal = self.body(insn, temps)
        return " ".join(temps + [body]), terminal

    def body(self, insn, temps):
        m = insn.mnemonic.removeprefix("notrack ").removeprefix("bnd ").removeprefix("lock ")
        ops = insn.operands
        here = insn.address
        nxt = here + insn.size
        opsize16 = 0x66 in insn.prefix
        if 0x67 in insn.prefix:
            raise EmitError("16-bit addressing is not supported")

        prefix = ""
        for p in ("rep ", "repe ", "repz ", "repne ", "repnz "):
            if m.startswith(p):
                prefix, m = p.strip(), m[len(p):]
        base = m[:-1] if m[-1:] in ("b", "w", "d") else m
        if base in STRING_OPS and m not in ("movsx", "movzx"):
            return self.string(insn, base, m[-1], prefix), False
        if prefix:
            raise EmitError(f"prefix {prefix} on {m}")

        if m in ("nop", "wait", "fwait", "pause"):
            return "", False
        if m.startswith("f") or insn.bytes[self._opcode_index(insn)] in range(0xD8, 0xE0):
            return self.x87(insn, temps), False
        if m in UNSUPPORTED:
            raise EmitError("privileged, BCD or far-jump instruction")

        if m == "mov":
            src, _, _ = self.operand(insn, ops[1], temps)
            _, put, _ = self.operand(insn, ops[0], temps)
            return put(src), False
        if m in ("movsx", "movzx"):
            src, _, sbits = self.operand(insn, ops[1], temps)
            _, put, dbits = self.operand(insn, ops[0], temps)
            if m == "movzx":
                return put(src), False
            return put(f"(uint{dbits}_t)(int{dbits}_t)(int{sbits}_t)({src})"), False
        if m in ARITH or m in LOGIC:
            a, put, bits = self.operand(insn, ops[0], temps)
            b, _, _ = self.operand(insn, ops[1], temps, bits)
            if m in ARITH:
                helper, carry = ARITH[m]
                expr = f"sub<{bits}>(c, {a}, {b}, {carry or 'false'})" if helper == "sub" else \
                    f"add<{bits}>(c, {a}, {b}, {carry or 'false'})"
            else:
                expr = f"logic<{bits}>(c, (uint64_t)({a}) {LOGIC[m]} (uint64_t)({b}))"
            if m in ("cmp", "test"):
                return f"(void){expr};", False
            if m == "xor" and ops[0].type == x86.X86_OP_REG and ops[1].type == x86.X86_OP_REG and ops[0].reg == ops[1].reg:
                return put(f"logic<{bits}>(c, 0)"), False
            return put(expr), False
        if m in ("inc", "dec", "neg"):
            a, put, bits = self.operand(insn, ops[0], temps)
            return put(f"{m}<{bits}>(c, {a})"), False
        if m == "not":
            a, put, bits = self.operand(insn, ops[0], temps)
            return put(f"~({a})"), False
        if m in SHIFTS:
            a, put, bits = self.operand(insn, ops[0], temps)
            count, _, _ = self.operand(insn, ops[1], temps, 8)
            return put(f"{SHIFTS[m]}<{bits}>(c, {a}, (unsigned)({count}))"), False
        if m in ("shld", "shrd"):
            a, put, bits = self.operand(insn, ops[0], temps)
            b, _, _ = self.operand(insn, ops[1], temps)
            count, _, _ = self.operand(insn, ops[2], temps, 8)
            return put(f"{m}<{bits}>(c, {a}, {b}, (unsigned)({count}))"), False
        if m in ("mul", "imul", "div", "idiv"):
            if len(ops) == 1:
                a, _, bits = self.operand(insn, ops[0], temps)
                if m in ("div", "idiv"):
                    return f"{m}{bits}(m, {hexu(here)}, {a});", False
                return f"{m}{bits}(c, {a});", False
            if m != "imul":
                raise EmitError(f"form of {m}")
            _, put, bits = self.operand(insn, ops[0], temps)
            a, _, _ = self.operand(insn, ops[-2], temps)
            b, _, _ = self.operand(insn, ops[-1], temps, bits)
            return put(f"imul2<{bits}>(c, {a}, {b})"), False
        if m == "lea":
            _, put, _ = self.operand(insn, ops[0], temps)
            return put(self.offset(insn, ops[1])), False
        if m == "xchg":
            a, put_a, _ = self.operand(insn, ops[0], temps)
            b, put_b, _ = self.operand(insn, ops[1], temps)
            return f"{{ const auto t = {a}; {put_a(b)} {put_b('t')} }}", False
        if m == "cbw":
            return "c.a.x = (uint16_t)(int16_t)(int8_t)c.a.l;", False
        if m == "cwde":
            return "c.a.e = (uint32_t)(int32_t)(int16_t)c.a.x;", False
        if m == "cwd":
            return "c.d.x = (c.a.x & 0x8000u) ? 0xFFFFu : 0u;", False
        if m == "cdq":
            return "c.d.e = (c.a.e & 0x80000000u) ? 0xFFFFFFFFu : 0u;", False
        if m == "push":
            a, _, bits = self.operand(insn, ops[0], temps)
            if ops[0].type == x86.X86_OP_REG and insn.reg_name(ops[0].reg) in SREG:
                return (f"m.push16({a});" if opsize16 else f"m.push32({a});"), False
            if ops[0].type == x86.X86_OP_IMM:
                bits = 16 if opsize16 else 32
                a = hexu(ops[0].imm & ((1 << bits) - 1))
            return (f"m.push16({a});" if bits == 16 else f"m.push32({a});"), False
        if m == "pop":
            sreg = ops[0].type == x86.X86_OP_REG and insn.reg_name(ops[0].reg) in SREG
            wide = not opsize16 if sreg else ops[0].size == 4
            value = "m.pop32()" if wide else "m.pop16()"
            # A memory destination is addressed after ESP has moved, as on the processor.
            late: list[str] = []
            _, put, _ = self.operand(insn, ops[0], late)
            return f"{{ const uint32_t t = {value}; {' '.join(late)} {put('t')} }}", False
        if m in ("pushal", "pushaw", "pusha", "pushad"):
            return ("pushaw(m);" if opsize16 else "pushad(m);"), False
        if m in ("popal", "popaw", "popa", "popad"):
            return ("popaw(m);" if opsize16 else "popad(m);"), False
        if m in ("pushfd", "pushf"):
            return ("m.push16((uint16_t)c.eflags());" if opsize16 or m == "pushf" else "m.push32(c.eflags());"), False
        if m in ("popfd", "popf"):
            if opsize16 or m == "popf":
                return "c.set_eflags((c.eflags() & 0xFFFF0000u) | m.pop16());", False
            return "c.set_eflags(m.pop32());", False
        if m == "lahf":
            return "c.a.h = (uint8_t)c.eflags();", False
        if m == "sahf":
            return "c.set_eflags((c.eflags() & ~0xD5u) | (c.a.h & 0xD5u));", False
        if m == "leave":
            if opsize16:
                return "c.sp.x = c.bp.x; c.bp.x = m.pop16();", False
            return "c.sp.e = c.bp.e; c.bp.e = m.pop32();", False
        if m == "enter":
            if ops[1].imm & 0x1F:
                raise EmitError("ENTER with a nesting level")
            return f"m.push32(c.bp.e); c.bp.e = c.sp.e; c.sp.e -= {hexu(ops[0].imm & 0xFFFF)};", False
        if m in ("les", "lds", "lfs", "lgs", "lss"):
            _, put, bits = self.operand(insn, ops[0], temps)
            address = self.address(insn, ops[1])
            s = SREG[m[1:]]
            if bits == 32:
                return (f"{{ const uint32_t a = {address}; const uint32_t o = m.rd32(a); "
                        f"m.load_seg({s}, m.rd16(a + 4)); {put('o')} }}"), False
            return (f"{{ const uint32_t a = {address}; const uint16_t o = m.rd16(a); "
                    f"m.load_seg({s}, m.rd16(a + 2)); {put('o')} }}"), False
        if m in ("bt", "bts", "btr", "btc"):
            return self.bit_test(insn, m, ops, temps), False
        if m in ("bsf", "bsr"):
            old, put, bits = self.operand(insn, ops[0], temps)
            src, _, _ = self.operand(insn, ops[1], temps)
            return put(f"{m}<{bits}>(c, {src}, {old})"), False
        if m.startswith("set") and m[3:] in CONDITIONS:
            _, put, _ = self.operand(insn, ops[0], temps)
            return put(f"{CONDITIONS[m[3:]]} ? 1 : 0"), False
        if m in ("clc", "stc", "cmc", "cli", "sti", "cld", "std"):
            return {"clc": "c.cf = false;", "stc": "c.cf = true;", "cmc": "c.cf = !c.cf;", "cli": "c.iflag = false;",
                    "sti": "c.iflag = true;", "cld": "c.df = false;", "std": "c.df = true;"}[m], False
        if m in ("in", "out"):
            value, port = (ops[0], ops[1]) if m == "in" else (ops[1], ops[0])
            p = "c.d.x" if port.type == x86.X86_OP_REG else hexu(port.imm & 0xFF)
            r, put, bits = self.reg(insn.reg_name(value.reg))
            if m == "in":
                return put(f"m.port_in({p}, {bits // 8})"), False
            return f"m.port_out({p}, {r}, {bits // 8});", False
        if m in ("lsl", "lar"):
            _, put, _ = self.operand(insn, ops[0], temps)
            sel, _, _ = self.operand(insn, ops[1], temps)
            query = "segment_limit" if m == "lsl" else "segment_rights"
            return f"{{ uint32_t v; c.zf = m.{query}((uint16_t)({sel}), &v); if (c.zf) {{ {put('v')} }} }}", False
        if m == "smsw":
            _, put, _ = self.operand(insn, ops[0], temps)
            return put("0x0011u"), False

        if m.startswith("j") and m[1:] in CONDITIONS:
            return f"if ({CONDITIONS[m[1:]]}) {{ {self.jump(here, ops[0].imm & 0xFFFFFFFF)} }}", False
        if m in ("jecxz", "jcxz"):
            reg = "c.c.e" if m == "jecxz" else "c.c.x"
            return f"if ({reg} == 0) {{ {self.jump(here, ops[0].imm & 0xFFFFFFFF)} }}", False
        if m in ("loop", "loope", "loopne"):
            extra = {"loop": "", "loope": " && c.zf", "loopne": " && !c.zf"}[m]
            return f"if (--c.c.e != 0{extra}) {{ {self.jump(here, ops[0].imm & 0xFFFFFFFF)} }}", False
        if m == "jmp":
            if ops[0].type == x86.X86_OP_IMM:
                return self.jump(here, ops[0].imm & 0xFFFFFFFF), True
            a, _, bits = self.operand(insn, ops[0], temps)
            if bits != 32:
                raise EmitError("16-bit indirect jump")
            return f"c.eip = {a}; return;", True
        if m == "call":
            if opsize16:
                raise EmitError("16-bit call")
            if ops[0].type == x86.X86_OP_IMM:
                return self.call("", nxt, ops[0].imm & 0xFFFFFFFF), False
            a, _, bits = self.operand(insn, ops[0], temps)
            if bits != 32:
                raise EmitError("far or 16-bit indirect call")
            return self.call(a, nxt, None), False
        if m == "ret":
            if opsize16:
                raise EmitError("16-bit return")
            extra = f" c.sp.e += {hexu(ops[0].imm & 0xFFFF)};" if ops else ""
            return f"c.eip = m.pop32();{extra} return;", True
        if m == "retf":
            if opsize16:
                raise EmitError("16-bit far return")
            extra = f" c.sp.e += {hexu(ops[0].imm & 0xFFFF)};" if ops else ""
            return f"c.eip = m.pop32(); m.load_seg(CS, (uint16_t)m.pop32());{extra} return;", True
        if m == "iretd":
            return "c.eip = m.pop32(); m.load_seg(CS, (uint16_t)m.pop32()); c.set_eflags(m.pop32()); return;", True
        if m == "int":
            return f"m.interrupt({hexu(ops[0].imm & 0xFF)}, {hexu(nxt)});", False
        if m == "int3":
            return f"m.interrupt(3u, {hexu(nxt)});", False
        if m == "int1":
            return f"m.interrupt(1u, {hexu(nxt)});", False
        if m == "into":
            return f"if (c.of) m.interrupt(4u, {hexu(nxt)});", False
        if m == "hlt":
            return f"c.eip = {hexu(nxt)}; m.halt();", False
        raise EmitError(f"instruction {m}")

    def bit_test(self, insn, m, ops, temps):
        dst, src = ops
        if dst.type == x86.X86_OP_MEM and src.type == x86.X86_OP_REG:
            bits = dst.size * 8
            offset, _, _ = self.reg(insn.reg_name(src.reg))
            address = self.address(insn, dst)
            shift = 5 if bits == 32 else 4
            signed = f"(int{bits}_t)({offset})"
            temps.append(f"const uint32_t a0 = (uint32_t)({address} + (int32_t)({signed} >> {shift}) * {bits // 8});")
            value, put, bit = f"m.rd{bits}(a0)", (lambda v, b=bits: f"m.wr{b}(a0, (uint{b}_t)({v}));"), \
                f"((unsigned)({offset}) & {bits - 1}u)"
        else:
            value, put, bits = self.operand(insn, dst, temps)
            index, _, _ = self.operand(insn, src, temps, 8 if src.type == x86.X86_OP_IMM else None)
            bit = f"((unsigned)({index}) & {bits - 1}u)"
        head = f"const uint{bits}_t v = {value}; const unsigned k = {bit}; c.cf = ((v >> k) & 1) != 0;"
        change = {"bt": "", "bts": put(f"v | (uint{bits}_t(1) << k)"), "btr": put(f"v & ~(uint{bits}_t(1) << k)"),
                  "btc": put(f"v ^ (uint{bits}_t(1) << k)")}[m]
        return f"{{ {head} {change} }}"

    def string(self, insn, base, suffix, prefix):
        bits = {"b": 8, "w": 16, "d": 32}.get(suffix)
        if bits is None:
            raise EmitError(f"string instruction {base}{suffix}")
        source = SEG_PREFIX.get(insn.prefix[1], "DS")
        if base in ("movs", "stos", "lods", "ins", "outs"):
            mode = 1 if prefix else 0
        else:
            mode = {"": 0, "rep": 1, "repe": 1, "repz": 1, "repne": 2, "repnz": 2}[prefix]
        if base in ("stos", "scas", "ins"):
            return f"{base}<{bits}>(m, {mode});"
        return f"{base}<{bits}>(m, {source}, {mode});"

    # x87 -----------------------------------------------------------------------------------------------

    @staticmethod
    def _opcode_index(insn) -> int:
        i = 0
        while i < len(insn.bytes) - 1 and insn.bytes[i] in PREFIX_BYTES:
            i += 1
        return i

    def x87(self, insn, temps):
        """Translates an x87 instruction from its opcode bytes, which avoids operand-order ambiguity."""
        i = self._opcode_index(insn)
        opcode, modrm = insn.bytes[i], insn.bytes[i + 1] if i + 1 < len(insn.bytes) else 0
        if opcode < 0xD8 or opcode > 0xDF:
            raise EmitError("x87 instruction without an x87 opcode")
        reg, mod, rm = (modrm >> 3) & 7, modrm >> 6, modrm & 7
        if mod != 3:
            memory = next((o for o in insn.operands if o.type == x86.X86_OP_MEM), None)
            if memory is None:
                raise EmitError("x87 memory form without a memory operand")
            temps.append(f"const uint32_t a0 = {self.address(insn, memory)};")
            return self.x87_memory(opcode, reg)
        return self.x87_register(opcode, reg, rm, modrm)

    ARITH87 = ["+", "*", "com", "comp", "-", "-r", "/", "/r"]

    def arith87(self, kind: str, dst: str, src: str) -> str:
        if kind in ("+", "*", "-", "/"):
            return f"{dst} = {dst} {kind} {src};"
        return f"{dst} = {src} {kind[0]} {dst};"

    def x87_memory(self, opcode, reg):
        loads = {0xD8: "(double)f32(m.rd32(a0))", 0xDC: "f64(m.rd64(a0))",
                 0xDA: "(double)(int32_t)m.rd32(a0)", 0xDE: "(double)(int16_t)m.rd16(a0)"}
        if opcode in loads:
            kind = self.ARITH87[reg]
            src = loads[opcode]
            if kind == "com":
                return f"fcompare(c, st(c, 0), {src});"
            if kind == "comp":
                return f"fcompare(c, st(c, 0), {src}); (void)fpop(c);"
            return self.arith87(kind, "st(c, 0)", src)
        table = {
            (0xD9, 0): "fpush(c, (double)f32(m.rd32(a0)));",
            (0xD9, 2): "m.wr32(a0, bits32((float)st(c, 0)));",
            (0xD9, 3): "m.wr32(a0, bits32((float)fpop(c)));",
            (0xD9, 5): "c.fpu.cw = m.rd16(a0);",
            (0xD9, 7): "m.wr16(a0, c.fpu.cw);",
            (0xDB, 0): "fpush(c, (double)(int32_t)m.rd32(a0));",
            (0xDB, 2): "m.wr32(a0, (uint32_t)fist<int32_t>(c, st(c, 0)));",
            (0xDB, 3): "m.wr32(a0, (uint32_t)fist<int32_t>(c, fpop(c)));",
            (0xDB, 5): "fpush(c, read80(m, a0));",
            (0xDB, 7): "write80(m, a0, fpop(c));",
            (0xDD, 0): "fpush(c, f64(m.rd64(a0)));",
            (0xDD, 2): "m.wr64(a0, bits64(st(c, 0)));",
            (0xDD, 3): "m.wr64(a0, bits64(fpop(c)));",
            (0xDD, 4): "frstor(m, a0);",
            (0xDD, 6): "fnsave(m, a0);",
            (0xDD, 7): "m.wr16(a0, fstatus(c));",
            (0xDF, 0): "fpush(c, (double)(int16_t)m.rd16(a0));",
            (0xDF, 2): "m.wr16(a0, (uint16_t)fist<int16_t>(c, st(c, 0)));",
            (0xDF, 3): "m.wr16(a0, (uint16_t)fist<int16_t>(c, fpop(c)));",
            (0xDF, 5): "fpush(c, (double)(int64_t)m.rd64(a0));",
            (0xDF, 6): "fbstp(m, a0);",
            (0xDF, 7): "m.wr64(a0, (uint64_t)fist<int64_t>(c, fpop(c)));",
        }
        if (opcode, reg) not in table:
            raise EmitError(f"x87 memory form {opcode:02X} /{reg}")
        return table[(opcode, reg)]

    def x87_register(self, opcode, reg, i, modrm):
        sti = f"st(c, {i})"
        if opcode == 0xD8:
            kind = self.ARITH87[reg]
            if kind == "com":
                return f"fcompare(c, st(c, 0), {sti});"
            if kind == "comp":
                return f"fcompare(c, st(c, 0), {sti}); (void)fpop(c);"
            return self.arith87(kind, "st(c, 0)", sti)
        if opcode in (0xDC, 0xDE):
            pop = " (void)fpop(c);" if opcode == 0xDE else ""
            if opcode == 0xDE and modrm == 0xD9:
                return "fcompare(c, st(c, 0), st(c, 1)); (void)fpop(c); (void)fpop(c);"
            # Register forms of DC and DE write ST(i); E0/E8 and F0/F8 are the reversed pairs.
            kinds = {0: "+", 1: "*", 4: "-r", 5: "-", 6: "/r", 7: "/"}
            if reg in (2, 3) and opcode == 0xDC:
                pop = " (void)fpop(c);" if reg == 3 else ""
                return f"fcompare(c, st(c, 0), {sti});{pop}"
            if reg not in kinds:
                raise EmitError(f"x87 register form {opcode:02X} {modrm:02X}")
            return self.arith87(kinds[reg], sti, "st(c, 0)") + pop
        if opcode == 0xD9:
            if reg == 0:
                return f"{{ const double v = {sti}; fpush(c, v); }}"
            if reg == 1:
                return f"{{ const double v = st(c, 0); st(c, 0) = {sti}; {sti} = v; }}"
            simple = {
                0xD0: "", 0xE0: "st(c, 0) = -st(c, 0);", 0xE1: "st(c, 0) = std::fabs(st(c, 0));",
                0xE4: "fcompare(c, st(c, 0), 0.0);", 0xE8: "fpush(c, 1.0);", 0xE9: "fpush(c, 3.321928094887362348);",
                0xEA: "fpush(c, 1.442695040888963407);", 0xEB: "fpush(c, 3.141592653589793239);",
                0xEC: "fpush(c, 0.301029995663981195);", 0xED: "fpush(c, 0.693147180559945309);",
                0xEE: "fpush(c, 0.0);", 0xF0: "st(c, 0) = std::exp2(st(c, 0)) - 1.0;",
                0xF1: "{ const double x = fpop(c); st(c, 0) = st(c, 0) * std::log2(x); }",
                0xF2: "st(c, 0) = std::tan(st(c, 0)); fpush(c, 1.0); c.fpu.sw &= ~0x0400;",
                0xF3: "{ const double x = fpop(c); st(c, 0) = std::atan2(st(c, 0), x); }",
                0xF6: "c.fpu.top = (c.fpu.top + 7) & 7;", 0xF7: "c.fpu.top = (c.fpu.top + 1) & 7;",
                0xF8: "st(c, 0) = std::fmod(st(c, 0), st(c, 1)); c.fpu.sw &= ~0x0400;",
                0xF9: "{ const double x = fpop(c); st(c, 0) = st(c, 0) * std::log2(x + 1.0); }",
                0xFA: "st(c, 0) = std::sqrt(st(c, 0));",
                0xFB: "{ const double x = st(c, 0); st(c, 0) = std::sin(x); fpush(c, std::cos(x)); c.fpu.sw &= ~0x0400; }",
                0xFC: "st(c, 0) = fround(c, st(c, 0));",
                0xFD: "st(c, 0) = std::ldexp(st(c, 0), (int)std::trunc(st(c, 1)));",
                0xFE: "st(c, 0) = std::sin(st(c, 0)); c.fpu.sw &= ~0x0400;",
                0xFF: "st(c, 0) = std::cos(st(c, 0)); c.fpu.sw &= ~0x0400;",
            }
            if modrm in simple:
                return simple[modrm]
        if opcode == 0xDA and modrm == 0xE9:
            return "fcompare(c, st(c, 0), st(c, 1)); (void)fpop(c); (void)fpop(c);"
        if opcode == 0xDB and modrm == 0xE2:
            return "c.fpu.sw &= 0x7F00;"
        if opcode == 0xDB and modrm == 0xE3:
            return "finit(c);"
        if opcode == 0xDD:
            if reg == 0:
                return f"c.fpu.valid &= (uint8_t)~(1u << ((c.fpu.top + {i}) & 7));"
            if reg == 2:
                return f"{sti} = st(c, 0);"
            if reg == 3:
                return f"{{ const double v = st(c, 0); {sti} = v; (void)fpop(c); }}"
            if reg == 4:
                return f"fcompare(c, st(c, 0), {sti});"
            if reg == 5:
                return f"fcompare(c, st(c, 0), {sti}); (void)fpop(c);"
        if opcode == 0xDF and modrm == 0xE0:
            return "c.a.x = fstatus(c);"
        raise EmitError(f"x87 register form {opcode:02X} {modrm:02X}")

    # Output --------------------------------------------------------------------------------------------

    def chunk_source(self, index: int) -> str:
        addresses = self.chunks[index]
        start = self.starts[index]
        lines = [f"void {self.chunk_name(start)}(Machine& m) {{", "    Cpu& c = m.c;", "    switch (c.eip) {"]
        for position, address in enumerate(addresses):
            insn = self.found_insn(address)
            text = f"{insn.mnemonic} {insn.op_str}".strip()
            try:
                code, terminal = self.translate(insn)
            except EmitError as error:
                self.unsupported.append((address, f"{text}: {error}"))
                safe = text.replace("\\", "\\\\").replace('"', "'")
                code, terminal = f"m.unsupported({hexu(address)}, \"{safe}\");", True
            following = addresses[position + 1] if position + 1 < len(addresses) else None
            nxt = address + insn.size
            tail = "" if terminal or following == nxt else f" c.eip = {hexu(nxt)}; return;"
            lines.append(f"    case {hexu(address)}: {self.label(address)}: {{ {code}{tail} }} // {text}")
        lines += ["    default:", "        m.untranslated(c.eip);", "    }", "}", ""]
        return "\n".join(lines)

    def found_insn(self, address: int):
        obj = self.image.object_at(address)
        window = bytes(obj.data[address - obj.base:address - obj.base + 16])
        return next(self.disassembler.disasm(window, address, 1))


def write_program(image: le.LeImage, found: discover32.Discovery, source_name: str, source: bytes,
                  output: str, files: int) -> Emitter:
    emitter = Emitter(image, found)
    os.makedirs(output, exist_ok=True)
    header = [
        f"// Generated by tools/recomp/emit32.py from {source_name}. This file is derived from the original",
        "// program, so it stays on the machine of the user who owns that program and is never distributed.",
        "#pragma once",
        '#include "runtime/ops.h"',
        "namespace nax {",
    ]
    header += [f"void {emitter.chunk_name(s)}(Machine& m);" for s in emitter.starts]
    header += ["}  // namespace nax", ""]
    with open(os.path.join(output, "chunks.h"), "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(header))

    sources = [emitter.chunk_source(i) for i in range(len(emitter.starts))]
    total = sum(len(s) for s in sources)
    per_file = max(1, total // max(1, files) + 1)
    groups: list[list[str]] = [[]]
    size = 0
    for text in sources:
        if size >= per_file and groups[-1]:
            groups.append([])
            size = 0
        groups[-1].append(text)
        size += len(text)
    for old in os.listdir(output):
        if old.startswith("part_") and old.endswith(".cpp"):
            os.remove(os.path.join(output, old))
    for number, group in enumerate(groups):
        body = [
            f"// Generated by tools/recomp/emit32.py from {source_name}. Derived from the original program; never distributed.",
            '#include "chunks.h"',
            "#pragma warning(disable : 4102 4065)",
            "namespace nax {",
            "",
        ] + group + ["}  // namespace nax", ""]
        with open(os.path.join(output, f"part_{number:03d}.cpp"), "w", encoding="ascii", newline="\n") as f:
            f.write("\n".join(body))

    blob = bytearray()
    objects = []
    for obj in image.objects:
        objects.append((obj.base, obj.virtual_size, obj.flags, len(blob), len(obj.data)))
        blob += obj.data
    with open(os.path.join(output, "image.bin"), "wb") as f:
        f.write(blob)
    ends = []
    for index, start in enumerate(emitter.starts):
        last = emitter.chunks[index][-1]
        ends.append(last + emitter.found.instructions[last].size)
    table = [
        f"// Generated by tools/recomp/emit32.py from {source_name}. Derived from the original program; never distributed.",
        '#include "chunks.h"',
        "namespace nax {",
        "static const Chunk kChunks[] = {",
    ]
    table += [f"    {{{hexu(s)}, {hexu(e)}, {emitter.chunk_name(s)}}}," for s, e in zip(emitter.starts, ends)]
    table += ["};", "static const ObjectInfo kObjects[] = {"]
    table += [f"    {{{hexu(b)}, {hexu(s)}, {hexu(fl)}, {hexu(o)}, {hexu(n)}}}," for b, s, fl, o, n in objects]
    table += [
        "};",
        f"static const ProgramInfo kProgram = {{\"{source_name}\", {hexu(binascii.crc32(source))}, {hexu(len(source))}, "
        f"kChunks, {len(emitter.starts)}, kObjects, {len(objects)}, {hexu(image.entry_linear)}, "
        f"{hexu(image.stack_linear)}, \"image.bin\", {hexu(binascii.crc32(blob))}}};",
        "const ProgramInfo* translated_program() { return &kProgram; }",
        "}  // namespace nax",
        "",
    ]
    with open(os.path.join(output, "program.cpp"), "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(table))
    return emitter


def read_seeds(path: str | None) -> set[int]:
    seeds = set()
    if path and os.path.exists(path):
        with open(path, encoding="ascii") as f:
            for line in f:
                line = line.split("#")[0].strip()
                if line:
                    seeds.add(int(line, 16))
    return seeds


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("program")
    parser.add_argument("output")
    parser.add_argument("--seeds")
    parser.add_argument("--files", type=int, default=16)
    args = parser.parse_args()
    with open(args.program, "rb") as f:
        source = f.read()
    image = le.load(source)
    found = discover32.discover(image, read_seeds(args.seeds))
    emitter = write_program(image, found, os.path.basename(args.program), source, args.output, args.files)
    print(f"{len(found.instructions)} instructions in {len(emitter.starts)} chunks, "
          f"{len(emitter.unsupported)} left as run-time stops")
    report = os.path.join(args.output, "unsupported.txt")
    with open(report, "w", encoding="ascii", newline="\n") as f:
        for address, text in emitter.unsupported:
            f.write(f"{address:08x} {text}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
