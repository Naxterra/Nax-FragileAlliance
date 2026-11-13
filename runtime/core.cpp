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
        history[history_next++ & 15] = c.eip;
        const Chunk* chunk = find(c.eip);
        if (!chunk) untranslated(c.eip);
        chunk->fn(*this);
    }
}

std::string Machine::recent_path() const {
    std::string text = " Recently entered:";
    char part[16];
    for (unsigned i = 0; i < 16; i++) {
        const uint32_t a = history[(history_next + i) & 15];
        if (!a) continue;
        std::snprintf(part, sizeof part, " %08X", a - load_shift);
        text += part;
    }
    text += ". Stack:";
    for (uint32_t i = 0; i < 8; i++) {
        const uint32_t a = c.base[SS] + c.sp.e + 4 * i;
        // The stack may end at an uncommitted page; only committed memory is read.
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(mem + a, &info, sizeof info) || info.State != MEM_COMMIT ||
            static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize < mem + a + 4)
            break;
        uint32_t v;
        std::memcpy(&v, mem + a, 4);
        std::snprintf(part, sizeof part, " %08X", v >= load_shift ? v - load_shift : v);
        text += part;
    }
    return text + ".";
}

void Machine::untranslated(uint32_t address) {
    char text[128];
    std::snprintf(text, sizeof text, "The program reached address %08X, which was not translated.", address - load_shift);
    throw GuestFault(text + recent_path());
}

void Machine::unsupported(uint32_t address, const char* instruction) {
    char text[256];
    std::snprintf(text, sizeof text, "The program reached the instruction \"%s\" at %08X, which the translator does not support.",
                  instruction, address - load_shift);
    throw GuestFault(text);
}

void Machine::divide_error(uint32_t address) {
    char text[128];
    std::snprintf(text, sizeof text, "The program divided by zero or overflowed a division at %08X.", address - load_shift);
    throw GuestFault(text);
}

}  // namespace nax
