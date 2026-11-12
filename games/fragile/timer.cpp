// Native replacements of the timer module of Fragile Allegiance (original 0007050C to 000705F2). The original
// reprograms the timer chip and installs its own handler for the timer interrupt (IRQ 0). Here the original
// handler at 00070610 is called by the host at the requested rate from the Windows performance counter.
#include "games/fragile/game.h"

namespace nax::native {

namespace {
constexpr uint32_t kTimerHead = fa::A(0x12403C);      // head of the list of timer tasks; its fields start here
constexpr uint32_t kDivisor = fa::A(0x124044);        // timer chip divisor for the game's rate (head + 8)
constexpr uint32_t kTickStep = fa::A(0x124048);       // clock increment per tick (head + 0x0C)
constexpr uint32_t kBiosCountdown = fa::A(0x12405C);  // counts down to the next call of the old handler (head + 0x20)
constexpr uint32_t kBiosReload = fa::A(0x124058);     // divisor of the original 18.2 Hz BIOS clock (head + 0x1C)
constexpr uint32_t kOldVector = fa::A(0x124060);      // old IRQ 0 handler, called far by the game's handler (head + 0x24)
constexpr uint32_t kRunning = fa::A(0x12404C);
constexpr uint32_t kGameHandler = fa::A(0x70610);
constexpr uint32_t kIretd = fa::A(0x685FF);           // an IRETD in the original code, used as the "old handler"
constexpr double kChipHz = 1193181.667;
}  // namespace

// Original 0007050C: measures the timer chip's current divisor and saves the IRQ 0 vector. The BIOS rate of
// 65536 counts is reported, and the saved vector points to an IRETD so that the game's handler can still
// "chain" to the old handler 18.2 times a second without leaving translated code.
void timer_measure(Machine& m) {
    Game& g = game(m);
    g.wr32(kBiosReload, 0x10000);
    g.wr32(kOldVector, kIretd);
    g.wr16(kOldVector + 4, kCodeSelector);
    g.ret();
}

// Original 000705C4: stops the game's timer and gives IRQ 0 back to the BIOS.
void timer_stop(Machine& m) {
    Game& g = game(m);
    const uint32_t running = g.rd32(kRunning);
    g.wr32(kRunning, running & ~1u);
    if (running & 1) g.set_timer_interrupt(0, 0);
    g.ret();
}

// Original 00070564: starts the game's timer at EAX ticks per second. The list head and the step values are
// set exactly as the original sets them before it programs the chip.
void timer_start(Machine& m) {
    Game& g = game(m);
    Cpu& c = g.c;
    const uint32_t hz = c.a.e;
    // The original first calls the stop function.
    const uint32_t running = g.rd32(kRunning);
    g.wr32(kRunning, running & ~1u);
    g.set_timer_interrupt(0, 0);
    if (hz == 0) g.stop("The game started its timer with a rate of zero.");
    g.wr32(kTimerHead, kTimerHead);
    g.wr32(kTimerHead + 4, kTimerHead);
    uint32_t divisor = 0x2469B8u / hz;
    divisor = (divisor >> 1) + (divisor & 1);
    g.wr32(kDivisor, divisor);
    const uint64_t step = uint64_t(divisor) * 0x36ECEDu;
    g.wr32(kTickStep, uint32_t(step >> 16));
    g.wr32(kBiosCountdown, g.rd32(kBiosReload));
    g.set_timer_interrupt(kGameHandler, kChipHz / (divisor ? divisor : 0x10000));
    g.wr32(kRunning, g.rd32(kRunning) | 1u);
    g.log("timer started at %u Hz (divisor %u)", hz, divisor);
    g.ret();
}

// Site 0007063C in the game's timer handler: OUT 20h acknowledges the interrupt at the interrupt controller.
// The host needs no acknowledgement.
void acknowledge_interrupt(Machine& m) { (void)m; }

}  // namespace nax::native
