// Native replacement of the Watcom C start-up code of FRAGILE.EXE. The original asks DOS and the DOS/4GW
// extender for the version, the program segment prefix, the environment and the command line. Here the same
// library variables are filled from Windows, and then the original initialisers and main run as translated.
#include <cstdio>
#include <cstring>
#include <string>

#include "games/fragile/game.h"

namespace nax::native {

namespace {

void build_environment(Game& g, uint32_t* program_name_in_env) {
    // An environment block holds NUL-terminated "NAME=value" strings, an empty string, a word with the
    // count 1 and the full program name.
    uint32_t at = kEnvironmentBase;
    const char* entries[] = {"COMSPEC=C:\\COMMAND.COM", "PATH=C:\\"};
    for (const char* entry : entries) {
        g.write_string(at, entry);
        at += uint32_t(std::strlen(entry) + 1);
    }
    g.wr8(at++, 0);
    g.wr16(at, 1);
    at += 2;
    *program_name_in_env = at;
    g.write_string(at, "C:\\FRAGILE.EXE");
}

}  // namespace

void cstart(Machine& m) {
    Game& g = game(m);
    Cpu& c = g.c;
    c.sp.e &= ~3u;
    const uint32_t esp = c.sp.e;
    g.wr32(fa::kStackAtStart, esp);
    g.wr32(fa::kStackLow, esp);
    g.wr16(fa::kPspSelector, kPspSelector);
    // DOS 7.10 as under Windows 95, and DOS/4G with a zero-based flat data segment.
    g.wr8(fa::kDosMajor, 7);
    g.wr8(fa::kDosMinor, 10);
    g.wr8(fa::kExtenderType, 1);
    g.wr8(fa::kExtenderSubtype, 0);
    g.wr16(fa::kGetDsSelector, kDataSelector);

    uint32_t program_name = 0;
    build_environment(g, &program_name);
    g.wr32(fa::kEnvironmentOffset, kEnvironmentBase);
    g.wr16(fa::kEnvironmentSelector, kDataSelector);
    // A minimal program segment prefix, in case library code reads it through its selector.
    std::memset(g.mem + kPspBase, 0, 0x100);
    g.wr16(kPspBase, 0x20CD);
    g.wr16(kPspBase + 0x2C, kDataSelector);
    const std::string tail = g.arguments.substr(0, 126);
    g.wr8(kPspBase + 0x80, uint8_t(tail.size()));
    std::memcpy(g.mem + kPspBase + 0x81, tail.data(), tail.size());
    g.wr8(kPspBase + 0x81 + uint32_t(tail.size()), 0x0D);

    // The library expects the command line, a NUL, the program name and a NUL above the end of BSS.
    const uint32_t line = (fa::kBssEnd + 0xF) & ~0xFu;
    std::string trimmed = g.arguments;
    trimmed.erase(0, trimmed.find_first_not_of(' ') == std::string::npos ? trimmed.size() : trimmed.find_first_not_of(' '));
    g.write_string(line, trimmed);
    const uint32_t name = line + uint32_t(trimmed.size()) + 1;
    const std::string program = g.read_string(program_name);
    g.write_string(name, program);
    g.wr32(fa::kStringsEnd, name + uint32_t(program.size()));
    g.wr32(fa::kStackPointer, esp);
    g.wr8(fa::kNo87, 0);
    g.wr32(fa::kCommandLine, line);
    g.wr32(fa::kProgramName, name);

    c.a.e = 0xFF;
    g.call(fa::kRunInitialisers);
    c.bp.e = 0;
    g.call(fa::kCallMain);
    // The original main wrapper ends in exit(), which never returns.
    g.stop("The game's main function returned instead of ending the program.");
}

// Sites 0006BED8 and 0006BEED in the library's fatal exit: DOS 3D01h opens the console device "con" and DOS
// 40h writes the message to it. The message goes to the runtime log and to the console, if there is one.
void open_console(Machine& m) {
    m.c.a.e = 1;
    m.c.cf = false;
}

void write_console(Machine& m) {
    Game& g = game(m);
    const std::string text(reinterpret_cast<const char*>(g.mem + g.c.d.e), g.c.c.e);
    g.log("console: %s", text.c_str());
    std::fwrite(text.data(), 1, text.size(), stderr);
    g.c.a.e = g.c.c.e;
    g.c.cf = false;
}

// Site 0006BF03: DOS 4Ch ends the program with the code in AL, after the library ran its exit routines.
void terminate_program(Machine& m) {
    game(m).log("the game ended with code %u", m.c.a.l);
    throw ProgramExit{int(m.c.a.l)};
}

}  // namespace nax::native
