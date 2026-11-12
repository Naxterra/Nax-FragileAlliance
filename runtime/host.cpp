// The Windows host of a translated program.
#include "runtime/host.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace nax {

namespace {

struct MemoryFault {
    bool happened = false;
    uint64_t host_address = 0;
    bool write = false;
};
thread_local MemoryFault g_fault;

int fault_filter(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    g_fault.happened = true;
    g_fault.write = info->ExceptionRecord->ExceptionInformation[0] == 1;
    g_fault.host_address = info->ExceptionRecord->ExceptionInformation[1];
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

Host::Host() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpc_frequency_ = f.QuadPart;
    c.sel[CS] = kCodeSelector;
    for (int s : {ES, SS, DS, FS, GS}) c.sel[s] = kDataSelector;
}

Host::~Host() {
    if (log_) std::fclose(log_);
}

void Host::set_log(const std::filesystem::path& file) {
    if (log_) std::fclose(log_);
    log_ = _wfopen(file.c_str(), L"w");
}

void Host::log(const char* format, ...) {
    if (!log_) return;
    va_list args;
    va_start(args, format);
    std::vfprintf(log_, format, args);
    va_end(args);
    std::fputc('\n', log_);
    std::fflush(log_);
}

void Host::load(const std::filesystem::path& image_file) {
    const ProgramInfo* p = program();
    if (!p) throw std::runtime_error("No translated program is installed.");
    std::ifstream file(image_file, std::ios::binary);
    if (!file) throw std::runtime_error("The translated program image " + image_file.string() + " is missing.");
    const std::vector<char> image((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!commit(0, kLowMemoryEnd)) throw std::runtime_error("Committing low guest memory failed.");
    for (size_t i = 0; i < p->object_count; i++) {
        const ObjectInfo& o = p->objects[i];
        if (uint64_t(o.image_offset) + o.image_size > image.size() || o.image_size > o.size)
            throw std::runtime_error("The translated program image does not match the generated code.");
        if (!commit(o.base, o.size)) throw std::runtime_error("Committing guest memory for the program failed.");
        std::memcpy(mem + o.base, image.data() + o.image_offset, o.image_size);
    }
    for (int s = 0; s < 6; s++) c.base[s] = segment_base(c.sel[s]);
    c.sp.e = p->stack;
    c.eip = p->entry;
}

int Host::run() {
    failure_.clear();
    g_fault = {};
    const int code = run_protected();
    if (g_fault.happened) {
        char text[256];
        const uint64_t offset = g_fault.host_address - uint64_t(reinterpret_cast<uintptr_t>(mem));
        if (offset < 0x100000000ull)
            std::snprintf(text, sizeof text, "The program %s guest address %08X, which holds no memory, in %s.",
                          g_fault.write ? "wrote to" : "read from", unsigned(offset), where(c.eip).c_str());
        else
            std::snprintf(text, sizeof text, "The host failed with an access violation while running %s.",
                          where(c.eip).c_str());
        failure_ = text;
        return -1;
    }
    return code;
}

static int run_program(Host& host, std::string& failure) {
    try {
        const uint32_t sp0 = host.c.sp.e;
        host.push32(Machine::kNativeReturn);
        host.run_until(Machine::kNativeReturn, sp0);
        return int(host.c.a.e);
    } catch (const ProgramExit& exit) {
        return exit.code;
    } catch (const GuestFault& fault) {
        failure = fault.what();
        return -1;
    }
}

int Host::run_protected() {
    __try {
        return run_program(*this, failure_);
    } __except (fault_filter(GetExceptionInformation())) {
        return -1;
    }
}

void Host::set_timer_interrupt(uint32_t handler, double hz) {
    timer_handler_ = handler;
    if (!handler || hz <= 0) {
        timer_handler_ = 0;
        return;
    }
    timer_period_ = int64_t(double(qpc_frequency_) / hz);
    if (timer_period_ < 1) timer_period_ = 1;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    timer_next_ = now.QuadPart + timer_period_;
}

void Host::deliver_interrupt(uint32_t handler) {
    const uint32_t eip = c.eip;
    const uint32_t sp0 = c.sp.e;
    push32(c.eflags());
    push32(c.sel[CS]);
    push32(kNativeReturn);
    c.iflag = false;
    c.tf = false;
    c.eip = handler;
    interrupt_depth_++;
    run_until(kNativeReturn, sp0 - 8);
    interrupt_depth_--;
    c.eip = eip;
}

void Host::poll() {
    budget = 20000;
    if (!timer_handler_ || !c.iflag || interrupt_depth_ > 4) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (now.QuadPart < timer_next_) return;
    // After a long pause (a debugger, a moved window) the clock does not try to catch up on every tick.
    if (now.QuadPart - timer_next_ > 8 * timer_period_) timer_next_ = now.QuadPart;
    timer_next_ += timer_period_;
    deliver_interrupt(timer_handler_);
}

void Host::halt() {
    // HLT waits for the next interrupt. Without a timer nothing would ever come, so the wait is bounded.
    if (timer_handler_) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const int64_t wait = timer_next_ - now.QuadPart;
        if (wait > 0) {
            const DWORD ms = DWORD(wait * 1000 / qpc_frequency_);
            Sleep(ms > 50 ? 50 : ms);
        }
    }
    budget = 0;
    poll();
}

uint32_t Host::grow_heap(uint32_t size) {
    const uint32_t aligned = (size + 0xFFF) & ~0xFFFu;
    if (aligned < size || heap_top_ + uint64_t(aligned) > kHeapLimit) return 0;
    const uint32_t block = heap_top_;
    heap_top_ += aligned;
    if (heap_top_ > heap_committed_) {
        const uint32_t grow = ((heap_top_ - heap_committed_) + 0xFFFFF) & ~0xFFFFFu;
        if (!commit(heap_committed_, grow)) { heap_top_ = block; return 0; }
        heap_committed_ += grow;
    }
    return block;
}

std::string Host::read_string(uint32_t address, size_t max) const {
    std::string text;
    for (size_t i = 0; i < max; i++) {
        const char ch = char(rd8(address + uint32_t(i)));
        if (!ch) break;
        text += ch;
    }
    return text;
}

void Host::write_string(uint32_t address, const std::string& text) {
    std::memcpy(mem + address, text.c_str(), text.size() + 1);
}

std::string Host::where(uint32_t address) const {
    char text[96];
    if (const Chunk* chunk = find(address))
        std::snprintf(text, sizeof text, "the original function at %08X (address %08X)", chunk->start, address);
    else
        std::snprintf(text, sizeof text, "address %08X", address);
    return text;
}

void Host::stop(const std::string& message) {
    log("%s", message.c_str());
    throw GuestFault(message);
}

uint32_t Host::segment_base(uint16_t selector) {
    switch (selector & ~3u) {
    case 0:
    case kCodeSelector:
    case kDataSelector:
        return 0;
    case kPspSelector:
        return kPspBase;
    default: {
        char text[128];
        std::snprintf(text, sizeof text, "The program loaded selector %04X, which the host does not provide, in %s.",
                      selector, where(c.eip).c_str());
        stop(text);
    }
    }
}

uint16_t Host::object_selector(int object_index) {
    const ProgramInfo* p = program();
    if (p && object_index >= 1 && size_t(object_index) <= p->object_count &&
        (p->objects[object_index - 1].flags & 0x0004))
        return kCodeSelector;
    return kDataSelector;
}

void Host::interrupt(uint8_t n, uint32_t next_eip) {
    char text[200];
    std::snprintf(text, sizeof text,
                  "The program called INT %02Xh with AX=%04X in %s. This DOS, extender or BIOS service has no native "
                  "replacement yet.",
                  n, c.a.x, where(next_eip - 2).c_str());
    stop(text);
}

uint32_t Host::port_in(uint16_t port, int bytes) {
    (void)bytes;
    char text[160];
    std::snprintf(text, sizeof text, "The program read hardware port %04X in %s, which has no native replacement yet.",
                  port, where(c.eip).c_str());
    stop(text);
}

void Host::port_out(uint16_t port, uint32_t value, int bytes) {
    (void)bytes;
    char text[160];
    std::snprintf(text, sizeof text,
                  "The program wrote %X to hardware port %04X in %s, which has no native replacement yet.", value, port,
                  where(c.eip).c_str());
    stop(text);
}

}  // namespace nax
