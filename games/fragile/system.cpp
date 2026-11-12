// Native replacements of the system module of Fragile Allegiance (original 00068504 to 00068B4E): keyboard
// vectors, the random seed, Windows and CD-ROM detection, country settings and the critical error handler.
#include <windows.h>

#include "games/fragile/game.h"

namespace nax::native {

namespace {
constexpr uint32_t kKeyboardInstalled = 0x11F6DA;
constexpr uint32_t kOldKeyboardVector = 0x11F5C8;  // 6 bytes each, kept for the restore function
constexpr uint32_t kOldPrintScreenVector = 0x11F5CE;
constexpr uint32_t kRandomSeed = 0x11F858;
constexpr uint32_t kWindowsVersion = 0x11F85C;
constexpr uint32_t kDateFormat = 0x11F860, kDateSeparator = 0x11F861, kClock24 = 0x11F862, kTimeSeparator = 0x11F863;

uint8_t locale_char(LCTYPE type, char fallback) {
    wchar_t text[8] = {};
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, type, text, 8) > 0 && text[0] && text[0] < 128) return uint8_t(text[0]);
    return uint8_t(fallback);
}
}  // namespace

// Original 00068504: saves the keyboard (INT 9) and Print Screen (INT 5) vectors.
void save_keyboard_vectors(Machine& m) {
    Game& g = game(m);
    for (uint32_t i = 0; i < 12; i++) g.wr8(kOldKeyboardVector + i, 0);
    g.ret();
}

// Original 0006852D: puts the saved vectors back. The host never changed them.
void restore_keyboard_vectors(Machine& m) {
    game(m).keyboard_hooked = false;
    m.ret();
}

// Original 00068555: installs the game's keyboard interrupt handler at 00068581. Windows keyboard input is fed
// into the same key table by Game::key_event instead.
void install_keyboard(Machine& m) {
    Game& g = game(m);
    g.wr8(kKeyboardInstalled, 0xFF);
    g.keyboard_hooked = true;
    g.ret();
}

// Original 00068664: empties the BIOS keyboard buffer (INT 16h), waits until no key is held and stops passing
// keys on to the BIOS.
void flush_typed_keys(Machine& m) {
    Game& g = game(m);
    g.typed_keys.clear();
    g.wr8(fa::kPassKeysToBios, 0);
    g.ret();
}

// Original 000688C4: seeds the random generator from the timer chip, the clock and the date.
void seed_random(Machine& m) {
    Game& g = game(m);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    SYSTEMTIME now;
    GetLocalTime(&now);
    uint32_t seed = uint32_t(counter.QuadPart) ^ uint32_t(counter.QuadPart >> 32);
    seed += (uint32_t(now.wHour) * 3600 + now.wMinute * 60 + now.wSecond) * 100 + now.wMilliseconds / 10;
    seed += (uint32_t(now.wYear) << 16) | (uint32_t(now.wMonth) << 8) | now.wDay;
    seed = (seed & ~2u) | 1u;  // the original keeps the seed odd with bit 1 clear
    g.wr32(kRandomSeed, seed);
    g.ret();
}

// Original 00068A07: asks whether Windows runs (INT 2Fh 1600h). The game was tuned for plain DOS, so the
// answer is "no", as under DOSBox.
void windows_version(Machine& m) {
    Game& g = game(m);
    g.wr32(kWindowsVersion, 0);
    g.c.a.e = 0;
    g.ret();
}

// Original 00068A25: reads the country settings (INT 21h 3800h): date order, date and time separators and
// whether the clock shows 24 hours. They now come from the Windows regional settings.
void country_settings(Machine& m) {
    Game& g = game(m);
    const uint8_t order = locale_char(LOCALE_IDATE, '0');
    g.wr8(kDateFormat, uint8_t(order >= '0' && order <= '2' ? order - '0' : 0));
    g.wr8(kDateSeparator, locale_char(LOCALE_SDATE, '/'));
    g.wr8(kClock24, locale_char(LOCALE_ITIME, '0') == '1' ? 1 : 0);
    g.wr8(kTimeSeparator, locale_char(LOCALE_STIME, ':'));
    g.ret();
}

// Original 00068A9D: checks a drive letter in AL (0 for the current drive). It returns 0 when the drive does
// not exist, 1 for a normal drive and -1 for a CD-ROM drive, which the original asked MSCDEX about.
void check_drive(Machine& m) {
    Game& g = game(m);
    uint8_t letter = uint8_t(g.c.a.l);
    if (letter == 0) letter = uint8_t(g.files.current_drive());
    if (letter >= 'a' && letter <= 'z') letter = uint8_t(letter - 32);
    uint32_t result = 0;
    if (letter >= 'A' && letter <= 'Z' && g.files.has_drive(char(letter))) result = letter == 'D' ? 0xFFFFFFFFu : 1u;
    g.c.a.e = result;
    g.ret();
}

// Originals 00068B12 and 00068B3A: install and remove a critical error handler (INT 24h) that fails the call.
// Windows reports such errors to the file functions directly, so there is nothing to install.
void install_critical_handler(Machine& m) { m.ret(); }
void remove_critical_handler(Machine& m) { m.ret(); }

}  // namespace nax::native
