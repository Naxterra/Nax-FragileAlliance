// Native replacements used by the override tests in tests/test_emit32.py.
#include "runtime/cpu.h"

namespace nax::native {

// Replaces a whole original function: doubles EAX and returns as the original would.
void test_double(Machine& m) {
    m.c.a.e *= 2;
    m.ret();
}

// Replaces one instruction: sets EBX.
void test_site(Machine& m) { m.c.b.e = 0x77; }

}  // namespace nax::native
