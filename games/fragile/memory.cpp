// Native replacements of the Watcom C heap functions that ask the extender for memory.
#include <algorithm>

#include "games/fragile/game.h"

namespace nax::native {

// Original 0007D3FC: returns empty heap blocks to the extender (DPMI 0502h or DOS 49h). The host never takes
// memory back from the program, so the blocks simply stay in the heap's free list.
void heap_release_empty(Machine& m) {
    m.ret();
}

// Original 0007D61F: allocates a new heap block of EAX bytes from the extender (DPMI 0501h, or DOS 48h for
// small blocks). The block starts with the extender handle, a word that is zero for DPMI memory and the
// usable size; the function returns the address after this header, or 0 when no memory is left.
void heap_new_block(Machine& m) {
    Game& g = game(m);
    Cpu& c = g.c;
    const uint32_t size = c.a.e;
    const uint32_t block = size >= 8 ? g.grow_heap(size) : 0;
    if (block == 0) {
        c.a.e = 0;
    } else {
        g.wr32(block, block);  // the "handle" is only ever handed back to the release function above
        g.wr32(block + 4, 0);
        g.wr32(block + 8, size - 8);
        c.a.e = block + 8;
    }
    g.ret();
}

}  // namespace nax::native

namespace nax::native {

namespace {
constexpr uint32_t kInitHeapBlock = 0x6A3D6;  // EAX block, EDX size, EBX handle, ECX where to store the result
constexpr uint32_t kLargestBlock = 48u << 20;  // what the game gets when it asks for "as much as possible"

void new_game_heap(Game& g, uint32_t size, uint32_t limit) {
    Cpu& c = g.c;
    const uint32_t out = c.d.e;
    g.wr32(out, 0);
    uint32_t result = 0;
    if (size > limit) size = limit;
    if (size > 0x20) {
        const uint32_t block = g.grow_heap(size);
        if (block) result = g.invoke(kInitHeapBlock, block, size, block, out);
    }
    c.a.e = result;
    g.ret();
}
}  // namespace

// Originals 0006A2B4 and 0006A2D1: lock and unlock the 64 KB below the stack pointer with DPMI 0600h and
// 0601h. Nothing pages out under the host, so they do nothing.
void lock_stack(Machine& m) { m.ret(); }
void unlock_stack(Machine& m) { m.ret(); }

// Original 0006A2FE: the game's heap in conventional memory, from DPMI 0100h. EAX is the wanted size, or a
// negative number for as much as possible; EDX points to where the heap handle goes. The host has no
// conventional memory limit, so the block comes from the normal heap.
void game_heap_low(Machine& m) {
    Game& g = game(m);
    const int32_t wanted = int32_t(g.c.a.e);
    if (wanted == 0) { g.wr32(g.c.d.e, 0); g.c.a.e = 0; g.ret(); return; }
    const uint32_t paragraphs = wanted < 0 ? 0xFFFF : std::min<uint32_t>(0xFFFF, (uint32_t(wanted) + 0x60 + 0xF) >> 4);
    new_game_heap(g, paragraphs << 4, 0xFFFF0);
}

// Original 0006A35A: the game's heap in extended memory, from DPMI 0500h and 0501h, with the same arguments.
void game_heap_high(Machine& m) {
    Game& g = game(m);
    const int32_t wanted = int32_t(g.c.a.e);
    if (wanted == 0) { g.wr32(g.c.d.e, 0); g.c.a.e = 0; g.ret(); return; }
    new_game_heap(g, wanted < 0 ? kLargestBlock : uint32_t(wanted) + 0x60, kLargestBlock);
}

// Sites 0006A4C7 and 0006A4D6 in the game's heap release: DPMI 0101h and 0502h hand the memory back. The host
// keeps it, so the instructions do nothing.
void release_memory(Machine& m) { m.c.cf = false; }

}  // namespace nax::native
