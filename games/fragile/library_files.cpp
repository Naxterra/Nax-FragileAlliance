// Native replacements of the Watcom C library's file functions in FRAGILE.EXE. The library keeps its own
// table of per-handle flags (text or binary mode, access, append); the replacements keep using the original
// functions that read and write that table, so the rest of the library sees the same state as before.
#include <cstdio>
#include <string>
#include <vector>

#include "games/fragile/game.h"

namespace nax::native {

namespace {

constexpr uint32_t kSetErrno = fa::A(0x7D092);         // EAX = errno value
constexpr uint32_t kSetDosError = fa::A(0x7DBA1);      // EAX = DOS error; sets errno and returns -1
constexpr uint32_t kGetHandleFlags = fa::A(0x894AD);   // EAX = handle; returns the library's flags for it
constexpr uint32_t kSetHandleFlags = fa::A(0x89502);   // EAX = handle, EDX = flags
constexpr uint32_t kDefaultMode = fa::A(0x1210C1);     // _fmode: 0x200 means binary by default

constexpr uint32_t kFlagRead = 0x01, kFlagWrite = 0x02, kFlagBinary = 0x40, kFlagAppend = 0x80;
constexpr uint32_t kFlagDevice = 0x2000;
constexpr uint32_t kEbadf = 4, kEacces = 6, kEexist = 7, kEnomem = 5;

uint32_t fail_dos(Game& g, uint16_t error) { return g.invoke(kSetDosError, error); }
uint32_t fail_errno(Game& g, uint32_t value) {
    g.invoke(kSetErrno, value);
    return 0xFFFFFFFFu;
}

bool device(int handle) { return handle >= 0 && handle <= 4; }

}  // namespace

// Original 0008948F: isatty(handle) through DOS IOCTL 4400h. Only the five standard handles are devices.
void is_device(Machine& m) {
    m.c.a.e = device(int(m.c.a.e)) ? 1 : 0;
    m.ret();
}

// Original 00074D7D: _sopen(path, oflag, shflag, ...) with stack arguments. It opens, creates or truncates the
// file and records the access mode, append mode and text or binary mode in the library's handle table.
void sopen(Machine& m) {
    Game& g = game(m);
    Cpu& c = g.c;
    const uint32_t sp = c.sp.e;
    const std::string path = g.read_string(g.rd32(sp + 4));
    const uint32_t oflag = g.rd32(sp + 8);
    std::string trimmed = path;
    trimmed.erase(0, trimmed.find_first_not_of(' ') == std::string::npos ? trimmed.size() : trimmed.find_first_not_of(' '));
    const int access = int(oflag & 3);
    uint16_t error = 0;
    const int handle = g.files.open(trimmed, access == 3 ? 2 : access, (oflag & 0x20) != 0, (oflag & 0x40) != 0 && access != 0,
                                    (oflag & 0x400) != 0, &error);
    g.log("open \"%s\" flags %X -> %d (error %u)", trimmed.c_str(), oflag, handle, error);
    if (handle < 0) {
        c.a.e = error == kFileExists ? fail_errno(g, kEexist) : fail_dos(g, error);
        g.ret();
        return;
    }
    uint32_t flags = g.invoke(kGetHandleFlags, uint32_t(handle)) & 0x3C;
    if (device(handle)) flags |= kFlagDevice;
    flags |= access == 2 ? kFlagRead | kFlagWrite : access == 1 ? kFlagWrite : kFlagRead;
    if (oflag & 0x10) flags |= kFlagAppend;
    const uint32_t mode = (oflag >> 8) & 3;
    if (mode & 2 || (mode == 0 && g.rd32(kDefaultMode) == 0x200)) flags |= kFlagBinary;
    g.invoke(kSetHandleFlags, uint32_t(handle), flags);
    c.a.e = uint32_t(handle);
    g.ret();
}

// Original 0007D965: close(handle).
void close_file(Machine& m) {
    Game& g = game(m);
    const int handle = int(g.c.a.e);
    if (!g.files.close(handle, true)) {
        g.c.a.e = fail_errno(g, kEbadf);
    } else {
        g.invoke(kSetHandleFlags, uint32_t(handle), 0);
        g.c.a.e = 0;
    }
    g.ret();
}

// Original 0007DA13: the library's raw read(handle, buffer, count) without text conversion.
void read_raw(Machine& m) {
    Game& g = game(m);
    Cpu& c = g.c;
    uint16_t error = 0;
    const int64_t got = g.files.read(int(c.a.e), g.mem + c.d.e, c.b.e, &error);
    c.a.e = got < 0 ? fail_dos(g, error) : uint32_t(got);
    g.ret();
}

// Original 00074FE8: read(handle, buffer, count). In text mode carriage returns are dropped and a Ctrl+Z
// ends the file, as in the original.
void read_file(Machine& m) {
    Game& g = game(m);
    Cpu& c = g.c;
    const int handle = int(c.a.e);
    const uint32_t buffer = c.d.e, count = c.b.e;
    const uint32_t flags = g.invoke(kGetHandleFlags, uint32_t(handle));
    uint16_t error = 0;
    if (flags == 0) {
        c.a.e = fail_errno(g, kEbadf);
    } else if (!(flags & kFlagRead)) {
        c.a.e = fail_errno(g, kEacces);
    } else if (flags & kFlagBinary) {
        const int64_t got = g.files.read(handle, g.mem + buffer, count, &error);
        c.a.e = got < 0 ? fail_dos(g, error) : uint32_t(got);
    } else {
        uint32_t done = 0;
        std::vector<uint8_t> chunk;
        bool end = false;
        while (done < count && !end) {
            chunk.resize(count - done);
            const int64_t got = g.files.read(handle, chunk.data(), uint32_t(chunk.size()), &error);
            if (got < 0) { c.a.e = fail_dos(g, error); g.ret(); return; }
            if (got == 0) break;
            for (int64_t i = 0; i < got; i++) {
                const uint8_t ch = chunk[size_t(i)];
                if (ch == 0x1A) {
                    // Leave the file positioned at the Ctrl+Z, so later reads also see the end.
                    g.files.seek(handle, i - got, 1, &error);
                    end = true;
                    break;
                }
                if (ch != '\r') g.wr8(buffer + done++, ch);
            }
        }
        c.a.e = done;
    }
    g.ret();
}

// Original 00074FA7: lseek(handle, offset, whence).
void seek_file(Machine& m) {
    Game& g = game(m);
    Cpu& c = g.c;
    uint16_t error = 0;
    const int64_t at = g.files.seek(int(c.a.e), int32_t(c.d.e), int(c.b.e), &error);
    c.a.e = at < 0 ? fail_dos(g, error) : uint32_t(at);
    g.ret();
}

// Original 0007DFED: write(handle, buffer, count). Append mode writes at the end; text mode writes CR LF for
// every LF.
void write_file(Machine& m) {
    Game& g = game(m);
    Cpu& c = g.c;
    const int handle = int(c.a.e);
    const uint32_t buffer = c.d.e, count = c.b.e;
    const uint32_t flags = g.invoke(kGetHandleFlags, uint32_t(handle));
    uint16_t error = 0;
    if (flags == 0) { c.a.e = fail_errno(g, kEbadf); g.ret(); return; }
    if (!(flags & kFlagWrite)) { c.a.e = fail_errno(g, kEacces); g.ret(); return; }
    if (flags & kFlagAppend) g.files.seek(handle, 0, 2, &error);
    int64_t written;
    if (flags & kFlagBinary) {
        written = g.files.write(handle, g.mem + buffer, count, &error);
    } else {
        std::vector<uint8_t> text;
        text.reserve(count + count / 8);
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t ch = g.rd8(buffer + i);
            if (ch == '\n') text.push_back('\r');
            text.push_back(ch);
        }
        written = g.files.write(handle, text.data(), uint32_t(text.size()), &error);
        if (written == int64_t(text.size())) written = count;
    }
    if (written < 0) {
        c.a.e = fail_dos(g, error);
    } else {
        if (uint32_t(written) != count) g.invoke(kSetErrno, 12);  // disk full, as the original reports
        c.a.e = uint32_t(written);
    }
    g.ret();
}

// Original 00072DF5: getcwd(buffer, size). A null buffer is allocated with malloc, as in the original.
void get_cwd(Machine& m) {
    Game& g = game(m);
    Cpu& c = g.c;
    const char drive = g.files.current_drive();
    const std::string path = std::string(1, drive) + ":\\" + g.files.current_directory(drive);
    uint32_t buffer = c.a.e, size = c.d.e;
    if (buffer == 0) {
        size = uint32_t(path.size() + 1);
        buffer = g.invoke(fa::kMalloc, size);
        if (buffer == 0) {
            g.invoke(kSetErrno, kEnomem);
            c.a.e = 0;
            g.ret();
            return;
        }
    }
    for (uint32_t i = 0; i < size; i++) {
        const char ch = i < path.size() ? path[i] : 0;
        g.wr8(buffer + i, uint8_t(ch));
        if (!ch) break;
    }
    c.a.e = buffer;
    g.ret();
}

// Original 00072B9C: chdir(path).
void change_dir(Machine& m) {
    Game& g = game(m);
    const uint16_t error = g.files.change_directory(g.read_string(g.c.a.e));
    g.c.a.e = error ? fail_dos(g, error) : 0;
    g.ret();
}

// Original 00072B88: _dos_setdrive(drive, &count), drive 1 for A:.
void set_drive(Machine& m) {
    Game& g = game(m);
    const char letter = char('A' + g.c.a.e - 1);
    if (g.files.has_drive(letter)) g.files.set_current_drive(letter);
    g.wr32(g.c.d.e, 26);
    g.c.a.e = 0;
    g.ret();
}

// Original 0006F8AD: access(path, mode). Mode 2 asks whether the file may be written.
void access_file(Machine& m) {
    Game& g = game(m);
    GameFiles::Found found;
    bool read_only = false;
    if (!g.files.stat(g.read_string(g.c.a.e), &found, &read_only)) {
        g.c.a.e = fail_dos(g, kFileNotFound);
    } else if ((g.c.d.e & 2) && read_only) {
        g.c.a.e = fail_dos(g, kAccessDenied);
    } else {
        g.c.a.e = 0;
    }
    g.ret();
}

// Original 0006F8E8: _dos_getdiskfree(drive, &info). The game only checks that enough space is free.
void disk_free(Machine& m) {
    Game& g = game(m);
    const uint32_t info = g.c.d.e;
    g.wr16(info + 0, 0xFFFF);  // total clusters
    g.wr16(info + 2, 0x8000);  // available clusters
    g.wr16(info + 4, 64);      // sectors per cluster
    g.wr16(info + 6, 512);     // bytes per sector: one gigabyte free
    g.c.a.e = 0;
    g.ret();
}

}  // namespace nax::native

namespace nax::native {

// Original 00091BCB: the library's raw write(handle, buffer, count), with append mode. Text written to the
// standard handles goes to the runtime log and to the console, if there is one.
void write_raw(Machine& m) {
    Game& g = game(m);
    Cpu& c = g.c;
    const int handle = int(c.a.e);
    const uint32_t buffer = c.d.e, count = c.b.e;
    if (handle >= 0 && handle <= 4) {
        const std::string text(reinterpret_cast<const char*>(g.mem + buffer), count);
        g.log("console %d: %s", handle, text.c_str());
        std::fwrite(text.data(), 1, text.size(), handle == 2 ? stderr : stdout);
        c.a.e = count;
        g.ret();
        return;
    }
    const uint32_t flags = g.invoke(fa::A(0x894AD), uint32_t(handle));
    uint16_t error = 0;
    if (flags & 0x80) g.files.seek(handle, 0, 2, &error);
    const int64_t written = g.files.write(handle, g.mem + buffer, count, &error);
    if (written < 0) {
        c.a.e = g.invoke(fa::A(0x7DBA1), error);
    } else {
        if (uint32_t(written) != count) g.invoke(fa::A(0x7D092), 12);
        c.a.e = uint32_t(written);
    }
    g.ret();
}

}  // namespace nax::native

namespace nax::native {

// Sites in the movie player (00082000 to 00085FFF) that call DOS directly for its files. The one native
// function serves them all with the same file layer as the C library: open (3Dh), close (3Eh), read (3Fh),
// write (40h), seek (42h) and rename (56h), with DOS register conventions and the carry flag for errors.
void dos_file_call(Machine& m) {
    Game& g = game(m);
    Cpu& c = g.c;
    uint16_t error = 0;
    const uint8_t function = c.a.h;
    auto fail = [&](uint16_t code) {
        c.cf = true;
        c.a.e = code;
    };
    c.cf = false;
    switch (function) {
    case 0x3D: {
        const std::string name = g.read_string(c.base[DS] + c.d.e);
        const int access = c.a.l & 3;
        const int handle = g.files.open(name, access == 3 ? 2 : access, false, false, false, &error);
        g.log("open \"%s\" (movie player) -> %d (error %u)", name.c_str(), handle, error);
        if (handle < 0) fail(error); else c.a.e = uint32_t(handle);
        break;
    }
    case 0x3E:
        if (!g.files.close(int(c.b.x), true)) fail(kBadHandle);
        break;
    case 0x3F: {
        const int64_t got = g.files.read(int(c.b.x), g.mem + uint32_t(c.base[DS] + c.d.e), c.c.e, &error);
        if (got < 0) fail(error); else c.a.e = uint32_t(got);
        break;
    }
    case 0x40: {
        const int64_t done = g.files.write(int(c.b.x), g.mem + uint32_t(c.base[DS] + c.d.e), c.c.e, &error);
        if (done < 0) fail(error); else c.a.e = uint32_t(done);
        break;
    }
    case 0x42: {
        const int32_t offset = int32_t((uint32_t(c.c.x) << 16) | c.d.x);
        const int64_t at = g.files.seek(int(c.b.x), offset, c.a.l, &error);
        if (at < 0) {
            fail(error);
        } else {
            c.a.x = uint16_t(at);
            c.d.x = uint16_t(uint64_t(at) >> 16);
        }
        break;
    }
    case 0x56: {
        const std::string from = g.read_string(c.base[DS] + c.d.e), to = g.read_string(c.base[ES] + c.di.e);
        const uint16_t result = g.files.rename(from, to);
        if (result) fail(result);
        break;
    }
    default: {
        char text[96];
        std::snprintf(text, sizeof text, "The game called DOS function %02Xh, which has no native replacement.", function);
        g.stop(text);
    }
    }
}

}  // namespace nax::native
