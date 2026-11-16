// Guest address space and dispatcher of the machine that translated code runs on.
#include "runtime/cpu.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace nax {

namespace {

// The placeholder functions of Windows 10 1803 and later, which let a view replace part of a reservation.
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using UnmapViewOfFile2Fn = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

struct PlaceholderApi {
    VirtualAlloc2Fn alloc = nullptr;
    MapViewOfFile3Fn map = nullptr;
    UnmapViewOfFile2Fn unmap = nullptr;
    PlaceholderApi() {
        HMODULE kernel = GetModuleHandleW(L"kernelbase.dll");
        if (!kernel) return;
        alloc = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernel, "VirtualAlloc2"));
        map = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kernel, "MapViewOfFile3"));
        unmap = reinterpret_cast<UnmapViewOfFile2Fn>(GetProcAddress(kernel, "UnmapViewOfFile2"));
    }
    bool ok() const { return alloc && map && unmap; }
};

const PlaceholderApi& api() {
    static const PlaceholderApi instance;
    return instance;
}

}  // namespace

#ifdef NAX_TRACE_WRITES
uint32_t trace_range_lo();
uint32_t trace_range_hi();
#endif

Machine::Machine() {
#ifdef NAX_TRACE_WRITES
    trace_lo = trace_range_lo();
    trace_hi = trace_range_hi();
#endif
    const PlaceholderApi& p = api();
    if (!p.ok()) throw std::runtime_error("This version of Windows lacks the memory placeholder functions (Windows 10 1803 or later is needed).");
    // One placeholder covers the whole guest space. It is split into the part below the windows, the two
    // windows and the rest; the two outer parts become ordinary reservations that commit() fills.
    mem = static_cast<uint8_t*>(p.alloc(GetCurrentProcess(), nullptr, kReserved, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                        PAGE_NOACCESS, nullptr, 0));
    if (!mem) throw std::runtime_error("Reserving the 4 GB guest address space failed.");
    const uint32_t windows_end = kWindowB + kWindowSize;
    static_assert(kWindowB == kWindowA + kWindowSize, "the two windows must be next to each other");
    bool ok = VirtualFree(mem, kWindowA, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) &&
              VirtualFree(mem + kWindowA, kWindowSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) &&
              VirtualFree(mem + kWindowB, kWindowSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    ok = ok && p.alloc(GetCurrentProcess(), mem, kWindowA, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    ok = ok && p.alloc(GetCurrentProcess(), mem + windows_end, kReserved - windows_end, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
                       PAGE_NOACCESS, nullptr, 0);
    video_section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(kVideoSection >> 32),
                                        DWORD(kVideoSection), nullptr);
    ok = ok && video_section_;
    if (ok) vram = static_cast<uint8_t*>(MapViewOfFile(video_section_, FILE_MAP_ALL_ACCESS, 0, 0, kVideoMemory));
    ok = ok && vram;
    ok = ok && p.map(video_section_, GetCurrentProcess(), mem + kWindowA, 0, kWindowSize, MEM_REPLACE_PLACEHOLDER,
                     PAGE_READWRITE, nullptr, 0);
    ok = ok && p.map(video_section_, GetCurrentProcess(), mem + kWindowB, 0, kWindowSize, MEM_REPLACE_PLACEHOLDER,
                     PAGE_READWRITE, nullptr, 0);
    if (!ok) throw std::runtime_error("Setting up the guest address space with its video windows failed.");
    budget = 100000;
}

Machine::~Machine() {
    const PlaceholderApi& p = api();
    if (mem) {
        p.unmap(GetCurrentProcess(), mem + kWindowA, 0);
        p.unmap(GetCurrentProcess(), mem + kWindowB, 0);
        VirtualFree(mem, 0, MEM_RELEASE);
        VirtualFree(mem + kWindowB + kWindowSize, 0, MEM_RELEASE);
    }
    if (vram) UnmapViewOfFile(vram);
    if (video_section_) CloseHandle(video_section_);
}

bool Machine::map_window(int window, uint32_t bank) {
    window &= 1;
    if (bank == window_bank_[window]) return true;
    if ((uint64_t(bank) + 1) * kBankSize > kVideoMemory) return false;
    const PlaceholderApi& p = api();
    uint8_t* at = mem + (window ? kWindowB : kWindowA);
    if (!p.unmap(GetCurrentProcess(), at, MEM_PRESERVE_PLACEHOLDER)) return false;
    if (!p.map(video_section_, GetCurrentProcess(), at, uint64_t(bank) * kBankSize, kWindowSize, MEM_REPLACE_PLACEHOLDER,
               PAGE_READWRITE, nullptr, 0))
        return false;
    window_bank_[window] = bank;
    return true;
}

bool Machine::commit(uint32_t address, uint32_t size) {
    if (size == 0) return true;
    if (uint64_t(address) + size > 0x100000000ull) return false;
    // The two video windows are always backed by video memory, so a range across them is committed around them.
    const uint64_t end = uint64_t(address) + size;
    const uint32_t windows_end = kWindowB + kWindowSize;
    bool ok = true;
    if (address < kWindowA) {
        const uint32_t part_end = uint32_t(std::min<uint64_t>(end, kWindowA));
        ok = ok && VirtualAlloc(mem + address, part_end - address, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    }
    if (end > windows_end) {
        const uint32_t part_start = std::max(address, windows_end);
        ok = ok && VirtualAlloc(mem + part_start, size_t(end - part_start), MEM_COMMIT, PAGE_READWRITE) != nullptr;
    }
    return ok;
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

#ifdef NAX_TRACE_WRITES
#include <map>
#include <unordered_map>

namespace nax {
namespace {
std::unordered_map<uint64_t, uint64_t> trace_counts;  // (function << 32 | caller) to bytes written
uint64_t trace_total = 0;
}  // namespace

void Machine::trace_write(uint32_t) {
    trace_counts[(uint64_t(trace_function) << 32) | trace_caller]++;
    if ((++trace_total & 0xFFFFFF) == 0) trace_report();
}

// Writes the counts, most written first, to trace-writes.txt in the current folder.
void Machine::trace_report() {
    std::multimap<uint64_t, uint64_t, std::greater<uint64_t>> sorted;
    for (const auto& [key, count] : trace_counts) sorted.emplace(count, key);
    FILE* f = std::fopen("trace-writes.txt", "w");
    if (!f) return;
    for (const auto& [count, key] : sorted)
        std::fprintf(f, "%08X from %08X  %llu\n", uint32_t(key >> 32) - load_shift, uint32_t(key) - load_shift,
                     static_cast<unsigned long long>(count));
    std::fclose(f);
}

namespace {
// Reads NAX_TRACE_WRITES=lo-hi (guest addresses in hexadecimal) when the first machine is built.
struct TraceRange {
    uint32_t lo = 0, hi = 0;
    TraceRange() {
        if (const char* text = std::getenv("NAX_TRACE_WRITES")) {
            unsigned long a = 0, b = 0;
            if (std::sscanf(text, "%lx-%lx", &a, &b) == 2) { lo = uint32_t(a); hi = uint32_t(b); }
        }
    }
};
}  // namespace
uint32_t trace_range_lo() { static TraceRange r; return r.lo; }
uint32_t trace_range_hi() { static TraceRange r; return r.hi; }
}  // namespace nax
#endif
