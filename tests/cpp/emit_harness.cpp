// Runs a translated test program on the core machine and prints the outcome and a memory range as hex.
// Usage: emit_harness <folder with image.bin> <dump start in hex> <dump length in hex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "runtime/cpu.h"

namespace nax {
const ProgramInfo* translated_program();
}

namespace {

constexpr uint32_t kSentinel = 0xFFFFFFF0u;
constexpr uint32_t kStackTop = 0x90000u;

struct TestMachine : nax::Machine {
    void interrupt(uint8_t n, uint32_t) override {
        if (n == 0x21) c.a.e = 0xABCD0000u | c.a.x;
    }
    uint32_t port_in(uint16_t port, int) override { return port == 0x60 ? 0x5Au : 0xFFu; }
    void port_out(uint16_t port, uint32_t value, int) override { last_port = port; last_value = value; }
    uint16_t last_port = 0;
    uint32_t last_value = 0;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    TestMachine m;
    const nax::ProgramInfo* program = nax::translated_program();
    m.install(program);
    std::ifstream file(std::string(argv[1]) + "/" + program->image_file, std::ios::binary);
    const std::vector<char> image((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    for (size_t i = 0; i < program->object_count; i++) {
        const nax::ObjectInfo& o = program->objects[i];
        if (!m.commit(o.base, o.size) || o.image_offset + o.image_size > image.size()) return 3;
        std::memcpy(m.mem + o.base, image.data() + o.image_offset, o.image_size);
    }
    m.commit(kStackTop - 0x10000, 0x10000);
    m.c.sp.e = kStackTop;
    m.push32(kSentinel);
    m.c.eip = program->entry;
    try {
        m.run_until(kSentinel, kStackTop);
        std::printf("ok\n");
    } catch (const nax::GuestFault& fault) {
        std::printf("fault %s\n", fault.what());
    }
    std::printf("port %x %x\n", m.last_port, m.last_value);
    const uint32_t start = std::strtoul(argv[2], nullptr, 16), length = std::strtoul(argv[3], nullptr, 16);
    for (uint32_t i = 0; i < length; i++) std::printf("%02x", m.mem[start + i]);
    std::printf("\n");
    return 0;
}
