// Native replacements of the mouse module of Fragile Allegiance (original 0006EC04 to 0006EE06). The original
// talks to a DOS mouse driver through INT 33h and gets its handler at 0006ECC3 called on every mouse event.
// Here that handler is called by Game::take_input for each Windows mouse message.
#include "games/fragile/game.h"

namespace nax::native {

namespace {
constexpr uint32_t kInstalled = fa::A(0x120EB6);
constexpr uint32_t kDoubleSpeed = fa::A(0x120E70);  // motion threshold above which the handler doubles the motion
constexpr uint32_t kScaleX = fa::A(0x120E60), kScaleY = fa::A(0x120E64);
constexpr uint32_t kLastMickeysX = fa::A(0x120E68), kLastMickeysY = fa::A(0x120E6C);
constexpr uint32_t kHandler = fa::A(0x6ECC3);
}  // namespace

// Original 0006EC04: finds the mouse driver, reads its sensitivity and installs the event handler. Returns -1.
// The sensitivity is the driver default of 8 mickeys per 8 pixels in both directions, and doubling is off, so
// one pixel of Windows pointer motion moves the game pointer by one pixel.
void mouse_init(Machine& m) {
    Game& g = game(m);
    g.wr8(kInstalled, 1);
    g.wr16(kDoubleSpeed, 0x7FFF);
    g.wr32(kScaleX, 0x3333333u / 8);
    g.wr32(kScaleY, 0x4444444u / 8);
    for (uint32_t a : {kLastMickeysX, kLastMickeysY, fa::kMouseUnitsX, fa::kMouseUnitsY, fa::kMouseX, fa::kMouseY}) g.wr32(a, 0);
    g.mickeys_x = g.mickeys_y = 0;
    g.mouse_handler = kHandler;
    g.c.a.e = 0xFFFFFFFFu;
    g.ret();
}

// Original 0006ECAA: resets the driver, which removes the handler.
void mouse_shutdown(Machine& m) {
    Game& g = game(m);
    g.mouse_handler = 0;
    g.wr8(kInstalled, 0);
    g.ret();
}

// Site 0006ED74 in the handler: INT 33h function 0Bh reads and clears the driver's motion counters after an
// overflow. The handler only needs the counters cleared.
void mouse_clear_motion(Machine& m) {
    Game& g = game(m);
    g.mouse_motion_x = g.mouse_motion_y = 0;
    g.c.c.e = 0;
    g.c.d.e = 0;
}

}  // namespace nax::native
