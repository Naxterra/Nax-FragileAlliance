// Native replacements of Watcom C library functions in FRAGILE.EXE that call DOS or the DPMI host.
#include <windows.h>

#include "games/fragile/game.h"

namespace nax::native {

namespace fa_lib {
constexpr uint32_t kSetDosError = 0x7DB9C;  // stores the DOS error code after a failed call (EAX result, EDX carry)
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
