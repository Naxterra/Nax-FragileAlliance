// State shared by the native replacements of Fragile Allegiance's DOS, extender and hardware functions.
#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>

#include "runtime/files.h"
#include "runtime/host.h"

namespace nax {

// Addresses inside the original program that the native replacements read or write. They were found by
// reading the start-up code of the Watcom C library in FRAGILE.EXE.
namespace fa {
constexpr uint32_t kStackAtStart = 0x124454;
constexpr uint32_t kStackLow = 0x124440;
constexpr uint32_t kPspSelector = 0x12444C;
constexpr uint32_t kDosMajor = 0x124477;
constexpr uint32_t kDosMinor = 0x124478;
constexpr uint32_t kExtenderType = 0x12446E;
constexpr uint32_t kExtenderSubtype = 0x12446F;
constexpr uint32_t kGetDsSelector = 0x6BF11;  // selector word read by the library's __GETDS helper
constexpr uint32_t kEnvironmentOffset = 0x124471;
constexpr uint32_t kEnvironmentSelector = 0x124475;
constexpr uint32_t kNo87 = 0x12446C;
constexpr uint32_t kStringsEnd = 0x124450;
constexpr uint32_t kStackPointer = 0x12443C;
constexpr uint32_t kCommandLine = 0x124444;
constexpr uint32_t kProgramName = 0x124448;
constexpr uint32_t kBssEnd = 0x1EB920;

constexpr uint32_t kRunInitialisers = 0x73276;  // calls the start-up routines with a priority limit in EAX
constexpr uint32_t kCallMain = 0x73226;         // sets up the stack check and calls main, then exit
constexpr uint32_t kMalloc = 0x6F460;           // malloc(EAX bytes)
constexpr uint32_t kPassKeysToBios = 0x11F5D8;  // non-zero while typed keys also go to the BIOS buffer
}  // namespace fa

class Game : public Host {
public:
    std::filesystem::path data_dir;  // the original game folder, which is never written
    std::filesystem::path user_dir;  // saves, settings and the log
    std::string arguments;           // the command line passed to the game, without the program name
    GameFiles files;                 // drive C: is the game folder, drive D: the CD image
    bool keyboard_hooked = false;    // the game's keyboard handler would be installed
    std::deque<uint16_t> typed_keys; // BIOS-style keys (scan code and character) for INT 16h and getch
};

inline Game& game(Machine& m) { return static_cast<Game&>(m); }

}  // namespace nax
