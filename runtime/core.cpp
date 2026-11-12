// Guest address space and dispatcher of the machine that translated code runs on.
#include "runtime/cpu.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace nax {

Machine::Machine() {
    mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, kReserved, MEM_RESERVE, PAGE_NOACCESS));
    if (!mem) throw std::runtime_error("Reserving the 4 GB guest address space failed.");
    budget = 100000;
}

Machine::~Machine() {
    if (mem) VirtualFree(mem, 0, MEM_RELEASE);
}

bool Machine::commit(uint32_t address, uint32_t size) {
    if (size == 0) return true;
    if (uint64_t(address) + size > 0x100000000ull) return false;
    return VirtualAlloc(mem + address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

const Chunk* Machine::find(uint32_t address) const {
    if (!program_) return nullptr;
    const Chunk* first = program_->chunks;
    const Chunk* last = first + program_->chunk_count;
    // The chunks are sorted by start address and do not overlap.
    const Chunk* it = std::upper_bound(first, last, address, [](uint32_t a, const Chunk& k) { return a < k.start; });
    if (it == first) return nullptr;
    --it;
    return address < it->end ? it : nullptr;
}

void Machine::run_until(uint32_t ret, uint32_t sp0) {
    while (c.eip != ret || c.sp.e < sp0) {
        if (--budget < 0) poll();
        const Chunk* chunk = find(c.eip);
        if (!chunk) untranslated(c.eip);
        chunk->fn(*this);
    }
}

void Machine::untranslated(uint32_t address) {
    char text[128];
    std::snprintf(text, sizeof text, "The program reached address %08X, which was not translated.", address);
    throw GuestFault(text);
}

void Machine::unsupported(uint32_t address, const char* instruction) {
    char text[256];
    std::snprintf(text, sizeof text, "The program reached the instruction \"%s\" at %08X, which the translator does not support.",
                  instruction, address);
    throw GuestFault(text);
}

void Machine::divide_error(uint32_t address) {
    char text[128];
    std::snprintf(text, sizeof text, "The program divided by zero or overflowed a division at %08X.", address);
    throw GuestFault(text);
}

}  // namespace nax
