// Native replacements of Watcom C library functions in FRAGILE.EXE that call DOS or the DPMI host.
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "games/fragile/game.h"

namespace nax::native {

namespace fa_lib {
constexpr uint32_t kSetDosError = fa::A(0x7DB9C);  // stores the DOS error code after a failed call (EAX result, EDX carry)
}

// Original 000941C1: intdos(in, out). The game uses it only to read the clock (AH=2Ch), and the date
// (AH=2Ah) is answered as well. Any other DOS function stops the program with its number.
void intdos(Machine& m) {
    Game& g = game(m);
    Cpu& c = g.c;
    const uint32_t in = c.a.e, out = c.d.e;
    uint32_t regs[6];
    for (int i = 0; i < 6; i++) regs[i] = g.rd32(in + 4 * i);  // eax, ebx, ecx, edx, esi, edi
    const uint8_t function = uint8_t(regs[0] >> 8);
    SYSTEMTIME now;
    GetLocalTime(&now);
    if (function == 0x2C) {
        regs[2] = (regs[2] & 0xFFFF0000u) | (uint32_t(now.wHour) << 8) | now.wMinute;
        regs[3] = (regs[3] & 0xFFFF0000u) | (uint32_t(now.wSecond) << 8) | (now.wMilliseconds / 10);
    } else if (function == 0x2A) {
        regs[0] = (regs[0] & 0xFFFFFF00u) | now.wDayOfWeek;
        regs[2] = (regs[2] & 0xFFFF0000u) | now.wYear;
        regs[3] = (regs[3] & 0xFFFF0000u) | (uint32_t(now.wMonth) << 8) | now.wDay;
    } else {
        char text[96];
        std::snprintf(text, sizeof text, "The game called intdos with DOS function %02Xh, which has no native replacement.", function);
        g.stop(text);
    }
    for (int i = 0; i < 6; i++) g.wr32(out + 4 * i, regs[i]);
    g.wr32(out + 0x18, 0);  // carry clear
    // The original then records the result for errno and returns the output EAX.
    const uint32_t saved_ebx = c.b.e, saved_ecx = c.c.e, saved_esi = c.si.e, saved_edi = c.di.e;
    c.a.e = regs[0];
    c.d.e = 0;
    g.call(fa_lib::kSetDosError);
    c.b.e = saved_ebx;
    c.c.e = saved_ecx;
    c.si.e = saved_esi;
    c.di.e = saved_edi;
    c.a.e = regs[0];
    c.d.e = out;
    g.ret();
}

// Originals 000895E9 and 00089626: lock and unlock a linear memory range with DPMI 0600h and 0601h, so that
// interrupt handlers never page fault. Guest memory never pages out under the host, so both succeed at once.
void lock_region(Machine& m) {
    m.c.a.e = 0;
    m.ret();
}

void unlock_region(Machine& m) {
    m.c.a.e = 0;
    m.ret();
}

}  // namespace nax::native

namespace nax::native {

// Original 00091980: int386x(0x31, in, out, sregs) with stack arguments, the game's way to call the DPMI host.
// It is used by the network module, to ask for IPX (INT 2Fh 7A00h), IPX itself (INT 7Ah) and NetBIOS
// (INT 5Ch) through DPMI 0300h, and once to get DOS memory (DPMI 0100h). Networks are reported as absent,
// so the game runs as a single-player game, and DOS memory comes from conventional memory. Any other request
// stops the program with its function number.
void int386x_dpmi(Machine& m) {
    Game& g = game(m);
    const uint32_t sp = g.c.sp.e;
    const uint32_t in = g.rd32(sp + 8), out = g.rd32(sp + 12), sregs = g.rd32(sp + 16);
    uint32_t regs[6];  // eax, ebx, ecx, edx, esi, edi
    for (int i = 0; i < 6; i++) regs[i] = g.rd32(in + 4 * i);
    const uint16_t function = uint16_t(regs[0]);
    bool carry = false;
    char text[160];
    if (function == 0x0300) {
        const uint8_t number = uint8_t(regs[1]);
        const uint32_t call = regs[5];  // real-mode call structure, flat
        const uint32_t ax = g.rd32(call + 0x1C) & 0xFFFF;
        if (number == 0x2F && ax == 0x7A00) {
            g.wr8(call + 0x1C, 0x00);  // AL stays 0: no IPX driver
        } else if (number == 0x5C) {
            g.wr8(call + 0x1C, 0xFB);  // NetBIOS answers "no adapter"
            g.wr8(call + 0x1D, 0x00);
        } else {
            std::snprintf(text, sizeof text, "The game asked for real-mode interrupt %02Xh with AX=%04X, which has no native replacement.",
                          number, ax);
            g.stop(text);
        }
    } else if (function == 0x0100) {
        const uint32_t bytes = (regs[1] & 0xFFFF) * 16;
        if (bytes == 0 || g.conventional_next + bytes > 0xA0000) {
            carry = true;
            regs[0] = (regs[0] & 0xFFFF0000u) | 8;  // not enough memory
            regs[1] = (regs[1] & 0xFFFF0000u) | ((0xA0000 - g.conventional_next) >> 4);
        } else {
            const uint32_t block = g.conventional_next;
            g.conventional_next += (bytes + 15) & ~15u;
            std::memset(g.mem + block, 0, bytes);
            regs[0] = (regs[0] & 0xFFFF0000u) | (block >> 4);
            regs[3] = (regs[3] & 0xFFFF0000u) | g.dos_selector(block);
        }
    } else if (function == 0x0101) {
        // DOS memory is never handed back to a shared pool, so freeing always succeeds.
    } else {
        std::snprintf(text, sizeof text, "The game called DPMI function %04Xh, which has no native replacement.", function);
        g.stop(text);
    }
    for (int i = 0; i < 6; i++) g.wr32(out + 4 * i, regs[i]);
    (void)sregs;
    g.c.cf = carry;
    g.ret();
}

}  // namespace nax::native
