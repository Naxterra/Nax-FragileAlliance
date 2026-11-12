// The machine that translated 32-bit protected-mode code runs on: registers, flags, the x87 state, the flat
// 4 GB guest address space and the dispatcher that finds the translated code for an address. DOS, the
// extender and the PC hardware are provided by a subclass through the virtual service functions.
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace nax {

class Machine;

union Reg32 {
    uint32_t e;
    uint16_t x;
    struct {
        uint8_t l, h;
    };
};

enum Seg : int { ES = 0, CS = 1, SS = 2, DS = 3, FS = 4, GS = 5 };

struct Fpu {
    double st[8]{};
    unsigned top = 0;      // physical index of ST(0)
    uint16_t cw = 0x037F;  // control word after FNINIT
    uint16_t sw = 0;       // status word without the TOP field
    uint8_t valid = 0;     // one bit per physical register that holds a value
};

struct Cpu {
    Reg32 a{}, b{}, c{}, d{}, si{}, di{}, bp{}, sp{};
    uint32_t eip = 0;
    uint16_t sel[6]{};
    uint32_t base[6]{};
    bool cf = false, zf = false, sf = false, of = false, af = false, df = false, iflag = true, tf = false;
    // The parity flag is derived from the low byte of the last result only when an instruction reads it.
    uint8_t parity_byte = 1;
    Fpu fpu;

    bool pf() const {
        uint8_t v = parity_byte;
        v ^= v >> 4;
        v ^= v >> 2;
        v ^= v >> 1;
        return (v & 1) == 0;
    }
    void set_pf(bool on) { parity_byte = on ? 0 : 1; }

    uint32_t eflags() const {
        return 0x0002u | (cf ? 0x001u : 0) | (pf() ? 0x004u : 0) | (af ? 0x010u : 0) | (zf ? 0x040u : 0) |
               (sf ? 0x080u : 0) | (tf ? 0x100u : 0) | (iflag ? 0x200u : 0) | (df ? 0x400u : 0) | (of ? 0x800u : 0);
    }
    void set_eflags(uint32_t f) {
        cf = f & 0x001;
        set_pf(f & 0x004);
        af = f & 0x010;
        zf = f & 0x040;
        sf = f & 0x080;
        tf = f & 0x100;
        iflag = f & 0x200;
        df = f & 0x400;
        of = f & 0x800;
    }
};

// One translated function: a range of guest addresses and the C++ function that executes them.
using ChunkFn = void (*)(Machine&);
struct Chunk {
    uint32_t start, end;
    ChunkFn fn;
};

// An object of the original program, placed at its base address before the program starts.
struct ObjectInfo {
    uint32_t base, size, flags, image_offset, image_size;
};

// What the generator writes about one translated program.
struct ProgramInfo {
    const char* source;
    uint32_t source_crc, source_size;
    const Chunk* chunks;
    size_t chunk_count;
    const ObjectInfo* objects;
    size_t object_count;
    uint32_t entry, stack;
    const char* image_file;  // the object contents with fixups applied, written next to the generated code
    uint32_t image_crc;
};

// Thrown to end the program, for example by INT 21h function 4Ch. Unwinds every translated frame.
struct ProgramExit {
    int code;
};

// Thrown when the program cannot continue. The message names the guest address where possible.
struct GuestFault : std::runtime_error {
    explicit GuestFault(const std::string& message) : std::runtime_error(message) {}
};

class Machine {
public:
    static constexpr uint64_t kReserved = 0x100000000ull + 0x10000;

    Machine();
    virtual ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    Cpu c;
    uint8_t* mem = nullptr;  // base of the reserved guest address space
    int budget = 0;          // translated code calls poll() when this runs below zero

    // Memory. Every 32-bit address lies inside the reservation; touching a page that was never committed
    // is reported by the fault handler with the guest address.
    uint8_t rd8(uint32_t a) const { return mem[a]; }
    uint16_t rd16(uint32_t a) const { uint16_t v; std::memcpy(&v, mem + a, 2); return v; }
    uint32_t rd32(uint32_t a) const { uint32_t v; std::memcpy(&v, mem + a, 4); return v; }
    uint64_t rd64(uint32_t a) const { uint64_t v; std::memcpy(&v, mem + a, 8); return v; }
    void wr8(uint32_t a, uint8_t v) { mem[a] = v; }
    void wr16(uint32_t a, uint16_t v) { std::memcpy(mem + a, &v, 2); }
    void wr32(uint32_t a, uint32_t v) { std::memcpy(mem + a, &v, 4); }
    void wr64(uint32_t a, uint64_t v) { std::memcpy(mem + a, &v, 8); }

    // Stack. The stack segment of a flat program has base 0, but its base is still honoured.
    void push32(uint32_t v) { c.sp.e -= 4; wr32(c.base[SS] + c.sp.e, v); }
    uint32_t pop32() { const uint32_t v = rd32(c.base[SS] + c.sp.e); c.sp.e += 4; return v; }
    void push16(uint16_t v) { c.sp.e -= 2; wr16(c.base[SS] + c.sp.e, v); }
    uint16_t pop16() { const uint16_t v = rd16(c.base[SS] + c.sp.e); c.sp.e += 2; return v; }

    // Control flow. run_until executes translated code until the call that pushed return address ret, with
    // the stack pointer at sp0 before the push, has returned.
    void run_until(uint32_t ret, uint32_t sp0);
    void after_call(uint32_t ret, uint32_t sp0) {
        if (c.eip != ret || c.sp.e < sp0) run_until(ret, sp0);
    }
    void call_far(uint32_t ret, uint32_t sp0) { run_until(ret, sp0); }
    const Chunk* find(uint32_t address) const;
    [[noreturn]] void untranslated(uint32_t address);
    [[noreturn]] void unsupported(uint32_t address, const char* text);
    [[noreturn]] void divide_error(uint32_t address);

    // Segments. Selectors are resolved by the extender part of the runtime.
    void load_seg(int s, uint16_t selector) {
        c.sel[s] = selector;
        c.base[s] = segment_base(selector);
    }
    virtual uint32_t segment_base(uint16_t selector) { (void)selector; return 0; }
    virtual uint16_t object_selector(int object_index) { (void)object_index; return c.sel[DS]; }
    // LSL and LAR. The defaults describe a 4 GB flat data segment.
    virtual bool segment_limit(uint16_t selector, uint32_t* limit) { (void)selector; *limit = 0xFFFFFFFF; return true; }
    virtual bool segment_rights(uint16_t selector, uint32_t* rights) { (void)selector; *rights = 0x00CFF300; return true; }

    // Services supplied by the DOS and hardware part of the runtime.
    virtual void interrupt(uint8_t n, uint32_t next_eip) = 0;
    virtual uint32_t port_in(uint16_t port, int bytes) = 0;
    virtual void port_out(uint16_t port, uint32_t value, int bytes) = 0;
    virtual void poll() { budget = 100000; }
    virtual void halt() { poll(); }

    // Program set-up.
    void install(const ProgramInfo* program) { program_ = program; }
    const ProgramInfo* program() const { return program_; }
    bool commit(uint32_t address, uint32_t size);

protected:
    const ProgramInfo* program_ = nullptr;
};

}  // namespace nax
