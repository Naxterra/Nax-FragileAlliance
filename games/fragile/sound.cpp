// Native replacements of the HMI SOS sound library in FRAGILE.EXE. The library loads the DOS drivers
// HMIDRV.386 and HMIMDRV.386 and programs the sound card through them. Until the native sound output is
// written, the library reports that no sound card is present, which the game already handles: it prints
// "Failed" and plays without sound.
#include "games/fragile/game.h"

namespace nax::native {

// Original 000787B6: sosDIGIInitSystem(path, flags). A non-zero result means the system could not start.
void sound_digital_init_system(Machine& m) {
    game(m).log("digital sound is not available yet; the game runs without sound");
    m.c.a.e = 1;
    m.ret();
}

// Original 0007882F: sosDIGIUnInitSystem(). Nothing was started, so nothing has to stop.
void sound_digital_uninit_system(Machine& m) {
    m.c.a.e = 0;
    m.ret();
}

}  // namespace nax::native
