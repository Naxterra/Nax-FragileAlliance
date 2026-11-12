// Keyboard and mouse input of Fragile Allegiance. Windows messages arrive from the window thread and are fed
// into the game's own key table and the mouse state on the game thread, at the points where translated code
// polls for interrupts, which is where the original handlers would have run.
#include <windows.h>

#include "games/fragile/game.h"

namespace nax {

namespace {
constexpr uint32_t kLastKey = fa::A(0x11F5D4);     // last key code, with bit 7 for E0-prefixed keys
constexpr uint32_t kLastKeyDown = fa::A(0x11F5D5); // FFh when the last key was pressed, 0 when released
constexpr uint32_t kKeyTable = fa::A(0x11F5DA);    // one byte per key code: FFh while held
}  // namespace

// The same steps as the original handler at 00068581, with the byte the keyboard controller would deliver.
void Game::keyboard_byte(uint8_t byte) {
    if (byte == 0xFF || byte == 0xE1) return;
    const uint8_t previous = previous_key_byte_;
    previous_key_byte_ = byte;
    if (byte == 0xE0) return;
    if (previous == 0xE0 && (byte == 0x2A || byte == 0xAA)) return;  // the fake shifts of the cursor keys
    uint32_t code = byte & 0x7F;
    const uint8_t down = byte < 0x80 ? 0xFF : 0x00;
    if (code >= 0x60) return;
    if (previous == 0xE0) code |= 0x80;
    wr8(kLastKey, uint8_t(code));
    wr8(kLastKeyDown, down);
    // A key that is still held keeps its marks, so a reader's "seen" mark is not undone by key repeat.
    if (!((rd8(kKeyTable + code) & down) & 0x80)) wr8(kKeyTable + code, down);
}

// Calls the game's mouse handler at 0006ECC3 as the mouse driver would: AL holds the events, BL the buttons,
// CX and DX the driver's position and SI and DI its running motion counters. The handler turns motion into
// its own pointer position; afterwards that position is set to exactly where the Windows pointer is, so the
// game's pointer never drifts away from it.
void Game::mouse_event(uint32_t mask) {
    if (!mouse_handler) return;
    c.a.e = mask;
    c.b.e = uint32_t(mouse_buttons);
    c.c.e = uint32_t(mouse_x & 0x3FFF);
    c.d.e = uint32_t(mouse_y & 0x3FFF);
    c.si.e = mickeys_x;
    c.di.e = mickeys_y;
    call_far_handler(mouse_handler);
    const uint32_t w = display.width ? display.width.load() : 640, h = display.height ? display.height.load() : 480;
    wr32(fa::kMouseUnitsX, (uint32_t(mouse_x) * 65536u + w - 1) / w);
    wr32(fa::kMouseUnitsY, (uint32_t(mouse_y) * 65536u + h - 1) / h);
    wr16(fa::kMouseX, uint16_t(mouse_x));
    wr16(fa::kMouseY, uint16_t(mouse_y));
}

void Game::take_input() {
    // Input arrives where an interrupt could: never while interrupts are disabled or a handler runs.
    if (in_input_ || !c.iflag) return;
    in_input_ = true;
    InputEvent e;
    while (display.pop(&e)) {
        switch (e.kind) {
        case InputEvent::Key:
            if (e.extended) keyboard_byte(0xE0);
            keyboard_byte(uint8_t((e.scancode & 0x7F) | (e.down ? 0 : 0x80)));
            break;
        case InputEvent::Character:
            if (e.character < 256) typed_keys.push_back(uint16_t((e.scancode << 8) | e.character));
            while (typed_keys.size() > 32) typed_keys.pop_front();
            break;
        case InputEvent::MouseMove: {
            const int x = e.x, y = e.y;
            if (x == mouse_x && y == mouse_y) break;
            mickeys_x = uint16_t(mickeys_x + (x - mouse_x));
            mickeys_y = uint16_t(mickeys_y + (y - mouse_y));
            mouse_motion_x += x - mouse_x;
            mouse_motion_y += y - mouse_y;
            mouse_x = x;
            mouse_y = y;
            mouse_event(0x01);
            break;
        }
        case InputEvent::MouseButton: {
            const int b = e.button;
            if (b < 0 || b > 2) break;
            if (e.down) {
                mouse_buttons |= 1 << b;
                mouse_press_count[b]++;
                mouse_press_x[b] = mouse_x;
                mouse_press_y[b] = mouse_y;
            } else {
                mouse_buttons &= ~(1 << b);
                mouse_release_count[b]++;
                mouse_release_x[b] = mouse_x;
                mouse_release_y[b] = mouse_y;
            }
            // Event bits: 1 and 2 left, 3 and 4 right, 5 and 6 middle, each pressed then released.
            mouse_event(1u << (1 + 2 * b + (e.down ? 0 : 1)));
            break;
        }
        }
    }
    in_input_ = false;
}

void Game::poll() {
    Host::poll();
    take_input();
    if (display.quit && !quit_requested) {
        quit_requested = true;
        throw ProgramExit{0};
    }
}

uint32_t Game::port_in(uint16_t port, int bytes) {
    if (port == 0x3C9 && bytes == 1) {
        const uint8_t value = display.palette[dac_read_][dac_read_part_];
        if (++dac_read_part_ == 3) { dac_read_part_ = 0; dac_read_++; }
        return value;
    }
    if (port == 0x3DA) {
        // Bit 3: vertical retrace, bit 0: display disabled. The retrace is reported for the first 1/16 of
        // every 1/60 s, so code that waits for it waits about as long as on the original monitor.
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const int64_t phase = (now.QuadPart * 60) % frequency.QuadPart;
        const bool retrace = phase < frequency.QuadPart / 16;
        return retrace ? 0x09u : 0x00u;
    }
    return Host::port_in(port, bytes);
}

void Game::port_out(uint16_t port, uint32_t value, int bytes) {
    if (bytes == 1 && port == 0x3C8) { dac_write_ = uint8_t(value); dac_write_part_ = 0; return; }
    if (bytes == 1 && port == 0x3C7) { dac_read_ = uint8_t(value); dac_read_part_ = 0; return; }
    if (bytes == 1 && port == 0x3C9) {
        display.palette[dac_write_][dac_write_part_] = uint8_t(value & 0x3F);
        if (++dac_write_part_ == 3) { dac_write_part_ = 0; dac_write_++; }
        return;
    }
    Host::port_out(port, value, bytes);
}

}  // namespace nax
