// State shared by the native replacements of Fragile Allegiance's DOS, extender and hardware functions.
#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>

#include "runtime/display.h"
#include "runtime/files.h"
#include "runtime/host.h"

namespace nax {

// Addresses inside the original program that the native replacements read or write. They were found by
// reading the start-up code of the Watcom C library in FRAGILE.EXE.
namespace fa {
// The program is loaded 4 MB above its link addresses, as DOS/4GW loaded it above conventional memory: the
// game keeps fixed data in conventional memory (its "sim" heap lies at 60000h to 90000h). Addresses in
// this file and in the native replacements are link addresses, as the listing tool shows them.
constexpr uint32_t kShift = 0x400000;
constexpr uint32_t A(uint32_t link_address) { return link_address + kShift; }
constexpr uint32_t kStackAtStart = A(0x124454);
constexpr uint32_t kStackLow = A(0x124440);
constexpr uint32_t kPspSelector = A(0x12444C);
constexpr uint32_t kDosMajor = A(0x124477);
constexpr uint32_t kDosMinor = A(0x124478);
constexpr uint32_t kExtenderType = A(0x12446E);
constexpr uint32_t kExtenderSubtype = A(0x12446F);
constexpr uint32_t kGetDsSelector = A(0x6BF11);  // selector word read by the library's __GETDS helper
constexpr uint32_t kEnvironmentOffset = A(0x124471);
constexpr uint32_t kEnvironmentSelector = A(0x124475);
constexpr uint32_t kNo87 = A(0x12446C);
constexpr uint32_t kStringsEnd = A(0x124450);
constexpr uint32_t kStackPointer = A(0x12443C);
constexpr uint32_t kCommandLine = A(0x124444);
constexpr uint32_t kProgramName = A(0x124448);
constexpr uint32_t kBssEnd = A(0x1EB920);

constexpr uint32_t kRunInitialisers = A(0x73276);  // calls the start-up routines with a priority limit in EAX
constexpr uint32_t kCallMain = A(0x73226);         // sets up the stack check and calls main, then exit
constexpr uint32_t kMalloc = A(0x6F460);           // malloc(EAX bytes)
constexpr uint32_t kPassKeysToBios = A(0x11F5D8);  // non-zero while typed keys also go to the BIOS buffer
constexpr uint32_t kMouseUnitsX = A(0x120E10);     // pointer position as a 16.16 fraction of the screen width
constexpr uint32_t kMouseUnitsY = A(0x120E14);
constexpr uint32_t kMouseX = A(0x120E18);          // word: pointer position in pixels
constexpr uint32_t kMouseY = A(0x120E1C);
}  // namespace fa

class Game : public Host {
public:
    std::filesystem::path data_dir;  // the original game folder, which is never written
    std::filesystem::path user_dir;  // saves, settings and the log
    std::string arguments;           // the command line passed to the game, without the program name
    GameFiles files;                 // drive C: is the game folder, drive D: the CD image
    bool keyboard_hooked = false;    // the game's keyboard handler would be installed
    std::deque<uint16_t> typed_keys; // BIOS-style keys (scan code and character) for INT 16h and getch
    Display display;                 // the picture and the input shared with the window thread
    uint32_t bank[2] = {0, 0};       // the bank shown in each of the two VESA windows
    uint32_t conventional_next = 0x10000;  // next free byte of conventional memory below 640 KB
    bool quit_requested = false;     // the user closed the window

    // Mouse, in game pixels, as the INT 33h driver kept it.
    int mouse_x = 0, mouse_y = 0, mouse_buttons = 0;
    int mouse_min_x = 0, mouse_max_x = 639, mouse_min_y = 0, mouse_max_y = 479;
    int mouse_press_count[3] = {0, 0, 0}, mouse_release_count[3] = {0, 0, 0};
    int mouse_press_x[3] = {0, 0, 0}, mouse_press_y[3] = {0, 0, 0};
    int mouse_release_x[3] = {0, 0, 0}, mouse_release_y[3] = {0, 0, 0};
    int mouse_motion_x = 0, mouse_motion_y = 0;  // mickeys not yet read
    uint32_t mouse_handler = 0;      // the game's mouse event handler, called far like a driver callback
    uint16_t mickeys_x = 0, mickeys_y = 0;  // the driver's running motion counters

    // Takes the input that the window thread collected; called from poll().
    void take_input();
    // Feeds one byte from the keyboard controller into the game's key table, as its handler at 00068581 does.
    void keyboard_byte(uint8_t byte);
    void poll() override;
    // The palette is still written through the VGA DAC ports 3C7h to 3C9h in a few drawing routines, and the
    // movie code waits for the vertical retrace at 3DAh. These ports go to the display; every other port
    // stops the program as before.
    uint32_t port_in(uint16_t port, int bytes) override;
    void port_out(uint16_t port, uint32_t value, int bytes) override;

private:
    void mouse_event(uint32_t mask);
    uint8_t previous_key_byte_ = 0;
    bool in_input_ = false;
    uint8_t dac_write_ = 0, dac_read_ = 0, dac_write_part_ = 0, dac_read_part_ = 0;
};

inline Game& game(Machine& m) { return static_cast<Game&>(m); }

}  // namespace nax
