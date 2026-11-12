// Native replacements of the video module of Fragile Allegiance (original 00070788 to 00070E6E) and of its
// palette functions. The original detects VESA, sets modes through INT 10h and reaches the 640x480 picture
// through a 64 KB window at A0000h that shows one 64 KB bank of video memory at a time. Here video memory is a
// framebuffer in guest memory, and a bank switch swaps the window's contents with the framebuffer, as the
// video card's memory mapping would. The window thread shows the framebuffer, with the bank that is in the
// window taken from the window, through Direct3D 12.
#include <algorithm>
#include <cstring>

#include "games/fragile/game.h"

namespace nax::native {

namespace {
constexpr uint32_t kFramebuffer = 0x00C00000u;
constexpr uint32_t kFramebufferSize = 0x00200000u;  // room for 1280x1024 with 8 bits per pixel
constexpr uint32_t kWindow = 0xA0000u, kWindowSize = 0x10000u;

constexpr uint32_t kMode = fa::A(0x124098);           // word: current mode; its high byte marks a VESA mode
constexpr uint32_t kStartMode = fa::A(0x1240A0);      // word: the mode found at start
constexpr uint32_t kVideoLevel = fa::A(0x12409A);     // 0 unknown, 1 VGA, 2 VESA
constexpr uint32_t kPaletteSet = fa::A(0x12409B);
constexpr uint32_t kDosBufferSelector = fa::A(0x1240A4);
constexpr uint32_t kModeInfo = fa::A(0x1240C4);       // copy of the first 0x27 bytes of the VESA mode information
constexpr uint32_t kReadWindow = fa::A(0x1240EC), kWriteWindow = fa::A(0x1240F0);  // window numbers, 0 for A
constexpr uint32_t kReadBase = fa::A(0x1240F4), kWriteBase = fa::A(0x1240F8);      // linear addresses of the windows
constexpr uint32_t kBankCache = fa::A(0x1240FC);      // two words: the bank in each window, FFFF when unknown
constexpr uint32_t kSetBank = fa::A(0x124100), kGetBank = fa::A(0x124104);         // functions called to move a window
constexpr uint32_t kBankShift = fa::A(0x1240AC);      // log2 of the window granularity
constexpr uint32_t kWindowFunction = fa::A(0x124166);
constexpr uint32_t kVesaMode = fa::A(0x124118), kBankArgument = fa::A(0x12411C);
constexpr uint32_t kCriticalCount = fa::A(0x1242CE);  // non-zero while the screen is being changed
constexpr uint32_t kGeometry = fa::A(0x70C37);        // derives the screen layout from the mode and window fields
constexpr uint32_t kClearScreen = fa::A(0x70D34);
constexpr uint32_t kSetBankFunction = fa::A(0x70E14), kGetBankFunction = fa::A(0x70E42);
constexpr uint32_t kPalette = fa::A(0x1210E6);        // 768 bytes: the palette the game wants to show
constexpr uint32_t kFadeSteps = fa::A(0x1213E8);      // 768 entries of value and step, 8.8 fixed point
constexpr uint32_t kFadeCount = fa::A(0x1210E5), kFadeActive = fa::A(0x1210E4);
constexpr uint32_t kFadeStep = fa::A(0x6FA5E);

struct ModeSize {
    uint16_t mode, width, height;
};
constexpr ModeSize kVesaModes[] = {{0x100, 640, 400}, {0x101, 640, 480}, {0x103, 800, 600}, {0x105, 1024, 768}, {0x107, 1280, 1024}};

const ModeSize* vesa_mode(uint32_t mode) {
    for (const ModeSize& m : kVesaModes)
        if (m.mode == (mode & 0x7FFF)) return &m;
    return nullptr;
}

bool supported(uint32_t mode) {
    mode &= 0xFFFF;
    if (mode <= 7 || (mode >= 0x0D && mode <= 0x13)) return true;
    return vesa_mode(mode) != nullptr;
}

// Fills the VESA mode information fields that the rest of the module reads, for one 64 KB window at A0000h
// that is read and written, with a granularity of 64 KB.
void describe_mode(Game& g, const ModeSize& m) {
    for (uint32_t i = 0; i < 0x27; i++) g.wr8(kModeInfo + i, 0);
    g.wr16(kModeInfo + 0, 0x009B);  // supported, colour, graphics
    g.wr8(kModeInfo + 2, 0x07);     // window A exists, can be read and written
    g.wr16(kModeInfo + 4, 64);      // granularity in KB
    g.wr16(kModeInfo + 6, 64);      // window size in KB
    g.wr16(kModeInfo + 8, 0xA000);
    g.wr16(kModeInfo + 16, m.width);  // bytes per line
    g.wr16(kModeInfo + 18, m.width);
    g.wr16(kModeInfo + 20, m.height);
    g.wr8(kModeInfo + 25, 8);
    g.wr8(kModeInfo + 27, 4);  // packed pixel
    g.wr32(kReadWindow, 0);
    g.wr32(kWriteWindow, 0);
    g.wr32(kReadBase, kWindow);
    g.wr32(kWriteBase, kWindow);
    g.wr32(kBankCache, 0xFFFFFFFFu);
    g.wr32(kWindowFunction, 0);
    g.wr32(kSetBank, kSetBankFunction);
    g.wr32(kGetBank, kGetBankFunction);
    g.wr32(kBankShift, 16);
    g.bank[0] = g.bank[1] = 0;
    std::memset(g.mem + kFramebuffer, 0, kFramebufferSize);
    std::memset(g.mem + kWindow, 0, kWindowSize);
    g.display.window = kWindow;
    g.display.window_bank = 0;
}

// Shows bank in the window: the window's contents go back to the framebuffer and the new bank comes in.
void switch_bank(Game& g, uint32_t bank) {
    if (bank == g.bank[0] || (uint64_t(bank) + 1) * kWindowSize > kFramebufferSize) return;
    std::memcpy(g.mem + kFramebuffer + g.bank[0] * kWindowSize, g.mem + kWindow, kWindowSize);
    std::memcpy(g.mem + kWindow, g.mem + kFramebuffer + bank * kWindowSize, kWindowSize);
    g.bank[0] = g.bank[1] = bank;
    g.display.window_bank = bank;
}

void show_mode(Game& g, uint32_t mode) {
    Display& d = g.display;
    if (const ModeSize* m = vesa_mode(mode)) {
        d.framebuffer = kFramebuffer;
        d.banked = true;
        d.width = m->width;
        d.height = m->height;
        d.pitch = m->width;
        d.graphics = true;
    } else if ((mode & 0x7F) == 0x13) {
        d.framebuffer = 0xA0000;
        d.banked = false;
        d.width = 320;
        d.height = 200;
        d.pitch = 320;
        d.graphics = true;
    } else {
        d.graphics = false;  // text and planar modes are not shown
    }
    g.log("video mode %X", mode);
}
}  // namespace

// Original 00070788: remembers the start mode, detects VESA and prepares the screen layout.
void video_init(Machine& m) {
    Game& g = game(m);
    if (!g.commit(kFramebuffer, kFramebufferSize)) g.stop("Committing the framebuffer failed.");
    g.wr16(kStartMode, 3);
    g.wr16(kMode, 3);
    g.wr8(kVideoLevel, 2);
    g.wr16(kDosBufferSelector, 0);
    g.invoke(kGeometry, 0);
    g.ret();
}

// Original 0007081C: goes back to the start mode at the end.
void video_restore(Machine& m) {
    Game& g = game(m);
    g.wr8(kPaletteSet, 0);
    g.wr16(kMode, g.rd16(kStartMode));
    show_mode(g, g.rd16(kStartMode));
    g.ret();
}

// Original 00070861: whether the mode in AX can be set. Returns -1 when it can and 0 when it cannot.
void video_mode_supported(Machine& m) {
    m.c.a.e = supported(m.c.a.x) ? 0xFFFFFFFFu : 0u;
    m.ret();
}

// Original 00070909: sets the mode in AX with a black palette and an empty screen. Returns -1 on success.
void video_set_mode(Machine& m) {
    Game& g = game(m);
    const uint32_t mode = g.c.a.x;
    if (!supported(mode)) {
        g.c.a.e = 0;
        g.ret();
        return;
    }
    g.wr8(kCriticalCount, uint8_t(g.rd8(kCriticalCount) + 1));
    for (auto& entry : g.display.palette)
        for (auto& component : entry) component = 0;
    g.wr16(kMode, uint16_t(mode));
    if (const ModeSize* vm = vesa_mode(mode)) {
        g.wr16(kVesaMode, uint16_t(mode));
        describe_mode(g, *vm);
    }
    show_mode(g, mode);
    g.invoke(kGeometry, 0);
    g.invoke(kClearScreen, 0);
    g.display.enabled = true;
    g.wr8(kCriticalCount, uint8_t(g.rd8(kCriticalCount) - 1));
    g.c.a.e = 0xFFFFFFFFu;
    g.ret();
}

// Original 00070B5A: reads the VESA mode information for the current mode.
void video_mode_info(Machine& m) {
    Game& g = game(m);
    if (const ModeSize* vm = vesa_mode(g.rd16(kMode))) describe_mode(g, *vm);
    g.ret();
}

// Originals 00070E14 and 00070DD3: show bank AX in window BL. There is one window for reading and writing.
void video_set_bank(Machine& m) {
    Game& g = game(m);
    const uint32_t bank = g.c.a.x;
    g.wr16(kBankArgument, uint16_t(bank));
    switch_bank(g, bank);
    g.ret();
}

// Originals 00070E42 and 00070DF4: return the bank of window BL in EAX.
void video_get_bank(Machine& m) {
    Game& g = game(m);
    g.c.a.e = g.bank[0];
    g.ret();
}

// Original 00070D21: AL bit 5 turns the picture on, otherwise the screen stays black (VGA attribute
// controller).
void video_screen_on(Machine& m) {
    Game& g = game(m);
    g.display.enabled = (g.c.a.l & 0x20) != 0;
    g.ret();
}

// Original 0006FAC5: shows the 768-byte palette at 001210E6 (VGA DAC ports 3C8h and 3C9h).
void palette_show(Machine& m) {
    Game& g = game(m);
    for (int i = 0; i < 256; i++)
        for (int k = 0; k < 3; k++) g.display.palette[i][k] = uint8_t(g.rd8(kPalette + uint32_t(i * 3 + k)) & 0x3F);
    g.wr8(kPaletteSet, 0xFF);
    g.ret();
}

// Original 0006F9E6: starts a fade from the colours on screen to the palette at EAX (black when EAX is 0)
// over EDX time units. It reads the colours on screen from the VGA DAC; here they come from the display.
void palette_start_fade(Machine& m) {
    Game& g = game(m);
    Cpu& c = g.c;
    g.wr32(kFadeActive, g.rd32(kFadeActive) & ~1u);
    uint32_t target = c.a.e;
    const uint64_t scaled = uint64_t(c.d.e) << 16;
    uint32_t steps = uint32_t(std::min<uint64_t>(scaled / 0x140000u, 0xFF));
    if (steps == 0) steps = 1;  // the original would divide by zero here
    uint32_t entry = kFadeSteps;
    for (int i = 0; i < 768; i++) {
        const uint8_t now = uint8_t(g.display.palette[i / 3][i % 3] & 0x3F);
        g.wr16(entry, uint16_t((now << 8) | 0x80));
        int wanted = 0;
        if (target) wanted = g.rd8(target++) & 0x3F;
        const int step = ((wanted - now) * 256) / int(steps);
        g.wr16(entry + 2, uint16_t(int16_t(step)));
        entry += 4;
    }
    g.invoke(kFadeStep, 0);
    g.wr8(kFadeCount, uint8_t(steps - 1));
    g.wr32(kFadeActive, g.rd32(kFadeActive) | 1u);
    c.iflag = true;
    g.ret();
}

}  // namespace nax::native

namespace nax::native {

// Original 00072040: plays the movie named by EAX (.GDV files on the CD) and returns 0, or an error number.
// The movie player draws in VGA mode X, one plane at a time, which has no native drawing yet. Until it has,
// a movie is skipped the way the original skips a movie file that is missing, by returning 0.
void play_movie(Machine& m) {
    Game& g = game(m);
    g.log("movie \"%s\" skipped: native movie playback is not written yet", g.read_string(g.c.a.e).c_str());
    g.c.a.e = 0;
    g.ret();
}

}  // namespace nax::native
