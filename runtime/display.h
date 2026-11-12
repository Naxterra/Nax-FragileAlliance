// What the game thread shows and what the window thread reports back: the picture in guest memory with its
// palette, and the keyboard and mouse input. The window thread reads the picture without locking, as the
// video hardware read video memory; a torn frame is replaced by the next one 1/60 s later.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

namespace nax {

struct InputEvent {
    enum Kind : uint8_t { Key, Character, MouseMove, MouseButton } kind;
    uint8_t scancode = 0;  // PC set 1 scan code for Key
    bool extended = false;  // E0-prefixed key
    bool down = false;
    uint16_t character = 0;  // for Character: the typed character
    int x = 0, y = 0;        // for MouseMove: position in game pixels
    int button = 0;          // for MouseButton: 0 left, 1 right, 2 middle
};

struct Display {
    // Picture: width x height bytes at guest address framebuffer, pitch bytes per line, 8-bit indexed.
    std::atomic<uint32_t> framebuffer{0}, width{0}, height{0}, pitch{0};
    // In banked modes one 64 KB bank of the framebuffer lives in the window at that time.
    std::atomic<bool> banked{false};
    std::atomic<uint32_t> window{0}, window_bank{0};
    std::atomic<bool> graphics{false};  // false while the game is in a text mode
    std::atomic<bool> enabled{true};    // the game can blank the screen while it changes modes
    std::atomic<uint8_t> palette[256][3]{};  // 6 bits per component, as the VGA DAC holds them

    // Input, filled by the window thread and drained by the game thread.
    std::mutex input_mutex;
    std::deque<InputEvent> input;
    std::atomic<bool> quit{false};  // the user closed the window

    void push(const InputEvent& e) {
        std::lock_guard<std::mutex> lock(input_mutex);
        input.push_back(e);
    }
    bool pop(InputEvent* e) {
        std::lock_guard<std::mutex> lock(input_mutex);
        if (input.empty()) return false;
        *e = input.front();
        input.pop_front();
        return true;
    }
};

}  // namespace nax
