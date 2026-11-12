// The Windows host of a translated program. It lays out guest memory, maps the few selectors the program
// uses onto flat memory, grows the heap, and reports every place where the program would still talk to DOS,
// the extender or the hardware, because those places are meant to be replaced by native functions.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

#include "runtime/cpu.h"

namespace nax {

constexpr uint16_t kCodeSelector = 0x0010;
constexpr uint16_t kDataSelector = 0x0018;
constexpr uint16_t kPspSelector = 0x0024;

constexpr uint32_t kLowMemoryEnd = 0x00100000u;   // first megabyte, committed for compatibility
constexpr uint32_t kPspBase = 0x00002000u;        // program segment prefix built by the host
constexpr uint32_t kEnvironmentBase = 0x00003000u; // environment block built by the host
constexpr uint32_t kHeapBase = 0x01000000u;       // the heap grows upwards from here
constexpr uint32_t kHeapLimit = 0x40000000u;

class Host : public Machine {
public:
    Host();
    ~Host() override;

    // Copies the translated program's objects into guest memory and sets the registers for its entry.
    void load(const std::filesystem::path& image_file);
    // Runs the program from its entry point until it ends. Returns the exit code; on a fault the message is
    // kept in failure().
    int run();
    const std::string& failure() const { return failure_; }

    // Heap: returns the guest address of a new block of at least size bytes, or 0 when the limit is reached.
    uint32_t grow_heap(uint32_t size);

    // Calls an original function with register arguments in the Watcom order (EAX, EDX, EBX, ECX) and
    // returns its EAX. Every other register keeps the value it had before, as Watcom callers expect.
    uint32_t invoke(uint32_t address, uint32_t eax, uint32_t edx = 0, uint32_t ebx = 0, uint32_t ecx = 0) {
        const Cpu saved = c;
        c.a.e = eax;
        c.d.e = edx;
        c.b.e = ebx;
        c.c.e = ecx;
        call(address);
        const uint32_t result = c.a.e;
        const Fpu fpu = c.fpu;
        c = saved;
        c.fpu = fpu;
        c.a.e = result;
        return result;
    }

    // Guest strings.
    std::string read_string(uint32_t address, size_t max = 260) const;
    void write_string(uint32_t address, const std::string& text);

    // Names the original function that contains an address, for messages.
    std::string where(uint32_t address) const;
    [[noreturn]] void stop(const std::string& message);

    void log(const char* format, ...);
    void set_log(const std::filesystem::path& file);

    // Timer interrupt: calls the original handler, which ends with IRETD, hz times a second while interrupts
    // are enabled. A handler of 0 stops it.
    void set_timer_interrupt(uint32_t handler, double hz);
    // Calls an original interrupt handler as the processor would: flags, code selector and return address on
    // the stack, interrupts disabled until the handler's IRETD.
    void deliver_interrupt(uint32_t handler);
    // Calls an original handler that a driver would have called far (it ends with RETF), with the registers
    // set by the caller, and then restores every register, as a driver callback from an interrupt does.
    void call_far_handler(uint32_t handler);
    void poll() override;
    void halt() override;

    // Makes a selector whose base is a block of conventional memory, as DPMI 0100h returns one.
    uint16_t dos_selector(uint32_t base);
    uint32_t segment_base(uint16_t selector) override;
    uint16_t object_selector(int object_index) override;
    void interrupt(uint8_t n, uint32_t next_eip) override;
    uint32_t port_in(uint16_t port, int bytes) override;
    void port_out(uint16_t port, uint32_t value, int bytes) override;

private:
    int run_protected();
    uint32_t timer_handler_ = 0;
    int64_t timer_period_ = 0, timer_next_ = 0, qpc_frequency_ = 1;
    int interrupt_depth_ = 0;
    std::map<uint16_t, uint32_t> selectors_;  // selectors made by dos_selector
    uint32_t heap_top_ = kHeapBase;
    uint32_t heap_committed_ = kHeapBase;
    std::string failure_;
    FILE* log_ = nullptr;
};

}  // namespace nax
