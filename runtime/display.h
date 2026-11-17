// What the game thread shows and what the window thread reports back: the picture in guest memory with its
// palette, and the keyboard and mouse input. The window thread reads the picture without locking, as the
// video hardware read video memory; a torn frame is replaced by the next one 1/60 s later.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

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

// An enhanced picture that replaces one of the game's own pictures, such as a sprite frame. index and opaque
// describe the original as the game draws it; the window thread shows the enhanced picture only on the pixels
// where the game picture still shows the original, so anything drawn over it stays visible.
struct OverlayTexture {
    uint64_t key = 0;                    // identifies the original picture by its content
    uint64_t version = 0;                // grows each time a pack file is read, so a changed file replaces the old
    int width = 0, height = 0;           // size of the original in game pixels
    std::vector<uint8_t> index, opaque;  // width x height palette indices, and 1 where the original is drawn
    int hd_width = 0, hd_height = 0;     // size of the enhanced picture
    std::vector<uint32_t> hd;            // RGBA, red in the lowest byte, straight alpha
    uint8_t reference[256][3]{};         // the palette (8 bits per component) the enhanced picture was made under
    std::vector<std::pair<uint8_t, uint32_t>> used;  // palette indices of the original and how often each occurs
};

// One enhanced picture to draw over the game picture: its top-left corner and its size in game pixels. A
// size other than the original's means the game draws the picture scaled down, for distance.
struct Overlay {
    std::shared_ptr<const OverlayTexture> texture;
    float x = 0, y = 0, width = 0, height = 0;
};

struct Display {
    // Picture: width x height bytes at pixels, pitch bytes per line, 8-bit indexed. pixels points into the
    // host's view of video memory.
    std::atomic<const uint8_t*> pixels{nullptr};
    std::atomic<uint32_t> width{0}, height{0}, pitch{0};
    std::atomic<bool> graphics{false};  // false while the game is in a text mode
    std::atomic<bool> enabled{true};    // the game can blank the screen while it changes modes
    std::atomic<uint8_t> palette[256][3]{};  // 6 bits per component, as the VGA DAC holds them

    // A complete picture as the game thread hands it to the window thread: the pixels, the palette and the
    // enhanced pictures of the same moment. Reading video memory while the game draws into it showed half
    // drawn pictures, in which the enhanced pictures and the AI filter fell back to the original for single
    // frames and blinked; a frame is only taken when the game is not changing the screen.
    struct Frame {
        uint32_t width = 0, height = 0;
        bool shown = false;            // false while the screen is blank or in a text mode
        std::vector<uint8_t> pixels;   // width x height palette indices
        uint8_t palette[256][3]{};     // 6 bits per component
        std::vector<Overlay> overlays; // enhanced pictures in drawing order
        uint64_t serial = 0;           // counts the frames handed over
    };
    std::mutex frame_mutex;
    Frame frame;
    // Hands over f; f receives the previous frame, so its buffers can be used again.
    void publish(Frame& f) {
        std::lock_guard<std::mutex> lock(frame_mutex);
        f.serial = frame.serial + 1;
        std::swap(frame, f);
    }
    // Copies the newest frame into out when it is not the one numbered seen.
    bool take(Frame* out, uint64_t seen) {
        std::lock_guard<std::mutex> lock(frame_mutex);
        if (frame.serial == seen) return false;
        *out = frame;
        return true;
    }

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
