// Flag-exact 386 integer and x87 operations used by translated code. Each helper returns the result and sets
// CF, ZF, SF, OF and the parity source the way the original instruction does. Shift and rotate counts of zero
// leave the flags unchanged. AF is not modelled because the translated program never reads it.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "runtime/cpu.h"

namespace nax {

template <int Bits>
struct W;
template <>
struct W<8> {
    using T = uint8_t;
    using S = int8_t;
    static constexpr uint64_t mask = 0xFF, sign = 0x80;
};
template <>
struct W<16> {
    using T = uint16_t;
    using S = int16_t;
    static constexpr uint64_t mask = 0xFFFF, sign = 0x8000;
};
template <>
struct W<32> {
    using T = uint32_t;
    using S = int32_t;
    static constexpr uint64_t mask = 0xFFFFFFFF, sign = 0x80000000;
};

template <int B>
inline typename W<B>::T result(Cpu& c, uint64_t r) {
    const auto v = typename W<B>::T(r & W<B>::mask);
    c.zf = v == 0;
    c.sf = (v & W<B>::sign) != 0;
    c.parity_byte = uint8_t(v);
    return v;
}

template <int B>
inline typename W<B>::T add(Cpu& c, uint64_t a, uint64_t b, bool carry) {
    a &= W<B>::mask;
    b &= W<B>::mask;
    const uint64_t r = a + b + (carry ? 1 : 0);
    c.cf = r > W<B>::mask;
    c.of = ((a ^ r) & (b ^ r) & W<B>::sign) != 0;
    return result<B>(c, r);
}

template <int B>
inline typename W<B>::T sub(Cpu& c, uint64_t a, uint64_t b, bool borrow) {
    a &= W<B>::mask;
    b &= W<B>::mask;
    const uint64_t r = a - b - (borrow ? 1 : 0);
    c.cf = a < b + (borrow ? 1 : 0);
    c.of = ((a ^ b) & (a ^ r) & W<B>::sign) != 0;
    return result<B>(c, r);
}

template <int B>
inline typename W<B>::T logic(Cpu& c, uint64_t r) {
    c.cf = c.of = false;
    return result<B>(c, r);
}

template <int B>
inline typename W<B>::T inc(Cpu& c, uint64_t a) {
    c.of = (a & W<B>::mask) == W<B>::sign - 1;
    return result<B>(c, a + 1);
}

template <int B>
inline typename W<B>::T dec(Cpu& c, uint64_t a) {
    c.of = (a & W<B>::mask) == W<B>::sign;
    return result<B>(c, a - 1);
}

template <int B>
inline typename W<B>::T neg(Cpu& c, uint64_t a) {
    return sub<B>(c, 0, a, false);
}

template <int B>
inline typename W<B>::T shl(Cpu& c, uint64_t v, unsigned n) {
    n &= 0x1F;
    v &= W<B>::mask;
    if (n == 0) return typename W<B>::T(v);
    const uint64_t r = v << n;
    c.cf = ((r >> B) & 1) != 0;
    const auto out = result<B>(c, r);
    c.of = ((out & W<B>::sign) != 0) != c.cf;
    return out;
}

template <int B>
inline typename W<B>::T shr(Cpu& c, uint64_t v, unsigned n) {
    n &= 0x1F;
    v &= W<B>::mask;
    if (n == 0) return typename W<B>::T(v);
    c.cf = ((v >> (n - 1)) & 1) != 0;
    c.of = (v & W<B>::sign) != 0;
    return result<B>(c, v >> n);
}

template <int B>
inline typename W<B>::T sar(Cpu& c, uint64_t v, unsigned n) {
    n &= 0x1F;
    if (n == 0) return typename W<B>::T(v);
    const int64_t s = int64_t(typename W<B>::S(typename W<B>::T(v)));
    c.cf = ((s >> (n - 1)) & 1) != 0;
    c.of = false;
    return result<B>(c, uint64_t(s >> n));
}

template <int B>
inline typename W<B>::T rol(Cpu& c, uint64_t v, unsigned n) {
    n &= 0x1F;
    v &= W<B>::mask;
    if (n == 0) return typename W<B>::T(v);
    const unsigned k = n % B;
    const auto r = typename W<B>::T(((v << k) | (v >> ((B - k) % B))) & W<B>::mask);
    c.cf = (r & 1) != 0;
    c.of = ((r & W<B>::sign) != 0) != c.cf;
    return r;
}

template <int B>
inline typename W<B>::T ror(Cpu& c, uint64_t v, unsigned n) {
    n &= 0x1F;
    v &= W<B>::mask;
    if (n == 0) return typename W<B>::T(v);
    const unsigned k = n % B;
    const auto r = typename W<B>::T(((v >> k) | (v << ((B - k) % B))) & W<B>::mask);
    c.cf = (r & W<B>::sign) != 0;
    c.of = ((r & W<B>::sign) != 0) != ((r & (W<B>::sign >> 1)) != 0);
    return r;
}

template <int B>
inline typename W<B>::T rcl(Cpu& c, uint64_t v, unsigned n) {
    n &= 0x1F;
    if (n == 0) return typename W<B>::T(v);
    uint64_t r = v & W<B>::mask;
    bool cf = c.cf;
    for (unsigned i = 0; i < n % (B + 1); i++) {
        const bool out = (r & W<B>::sign) != 0;
        r = ((r << 1) | (cf ? 1 : 0)) & W<B>::mask;
        cf = out;
    }
    c.cf = cf;
    c.of = ((r & W<B>::sign) != 0) != cf;
    return typename W<B>::T(r);
}

template <int B>
inline typename W<B>::T rcr(Cpu& c, uint64_t v, unsigned n) {
    n &= 0x1F;
    if (n == 0) return typename W<B>::T(v);
    uint64_t r = v & W<B>::mask;
    bool cf = c.cf;
    c.of = ((r & W<B>::sign) != 0) != cf;
    for (unsigned i = 0; i < n % (B + 1); i++) {
        const bool out = (r & 1) != 0;
        r = (r >> 1) | (cf ? W<B>::sign : 0);
        cf = out;
    }
    c.cf = cf;
    return typename W<B>::T(r);
}

// SHLD and SHRD: dst is shifted and filled from src. Counts above the width leave the result undefined on a
// 386; the helpers then return the plain double shift, which matches later processors.
template <int B>
inline typename W<B>::T shld(Cpu& c, uint64_t dst, uint64_t src, unsigned n) {
    n &= 0x1F;
    dst &= W<B>::mask;
    if (n == 0) return typename W<B>::T(dst);
    const uint64_t wide = (dst << B) | (src & W<B>::mask);
    const uint64_t r = (wide << n) >> B;
    c.cf = ((dst >> (B - n)) & 1) != 0;
    const auto out = result<B>(c, r);
    c.of = ((out ^ dst) & W<B>::sign) != 0;
    return out;
}

template <int B>
inline typename W<B>::T shrd(Cpu& c, uint64_t dst, uint64_t src, unsigned n) {
    n &= 0x1F;
    dst &= W<B>::mask;
    if (n == 0) return typename W<B>::T(dst);
    const uint64_t wide = ((src & W<B>::mask) << B) | dst;
    const uint64_t r = wide >> n;
    c.cf = ((dst >> (n - 1)) & 1) != 0;
    const auto out = result<B>(c, r);
    c.of = ((out ^ dst) & W<B>::sign) != 0;
    return out;
}

// Multiplication and division with the accumulator.
inline void mul8(Cpu& c, uint8_t v) {
    c.a.x = uint16_t(c.a.l * v);
    c.cf = c.of = c.a.h != 0;
}
inline void mul16(Cpu& c, uint16_t v) {
    const uint32_t r = uint32_t(c.a.x) * v;
    c.a.x = uint16_t(r);
    c.d.x = uint16_t(r >> 16);
    c.cf = c.of = c.d.x != 0;
}
inline void mul32(Cpu& c, uint32_t v) {
    const uint64_t r = uint64_t(c.a.e) * v;
    c.a.e = uint32_t(r);
    c.d.e = uint32_t(r >> 32);
    c.cf = c.of = c.d.e != 0;
}
inline void imul8(Cpu& c, uint8_t v) {
    const int16_t r = int16_t(int8_t(c.a.l) * int8_t(v));
    c.a.x = uint16_t(r);
    c.cf = c.of = r != int16_t(int8_t(r));
}
inline void imul16(Cpu& c, uint16_t v) {
    const int32_t r = int32_t(int16_t(c.a.x)) * int16_t(v);
    c.a.x = uint16_t(r);
    c.d.x = uint16_t(uint32_t(r) >> 16);
    c.cf = c.of = r != int32_t(int16_t(r));
}
inline void imul32(Cpu& c, uint32_t v) {
    const int64_t r = int64_t(int32_t(c.a.e)) * int32_t(v);
    c.a.e = uint32_t(r);
    c.d.e = uint32_t(uint64_t(r) >> 32);
    c.cf = c.of = r != int64_t(int32_t(r));
}
template <int B>
inline typename W<B>::T imul2(Cpu& c, uint64_t a, uint64_t b) {
    using S = typename W<B>::S;
    const int64_t r = int64_t(S(typename W<B>::T(a))) * int64_t(S(typename W<B>::T(b)));
    c.cf = c.of = r != int64_t(S(r));
    return typename W<B>::T(r);
}

inline void div8(Machine& m, uint32_t at, uint8_t v) {
    Cpu& c = m.c;
    if (v == 0 || c.a.x / v > 0xFF) m.divide_error(at);
    const uint16_t n = c.a.x;
    c.a.l = uint8_t(n / v);
    c.a.h = uint8_t(n % v);
}
inline void div16(Machine& m, uint32_t at, uint16_t v) {
    Cpu& c = m.c;
    const uint32_t n = (uint32_t(c.d.x) << 16) | c.a.x;
    if (v == 0 || n / v > 0xFFFF) m.divide_error(at);
    c.a.x = uint16_t(n / v);
    c.d.x = uint16_t(n % v);
}
inline void div32(Machine& m, uint32_t at, uint32_t v) {
    Cpu& c = m.c;
    const uint64_t n = (uint64_t(c.d.e) << 32) | c.a.e;
    if (v == 0 || n / v > 0xFFFFFFFFull) m.divide_error(at);
    c.a.e = uint32_t(n / v);
    c.d.e = uint32_t(n % v);
}
inline void idiv8(Machine& m, uint32_t at, uint8_t v) {
    Cpu& c = m.c;
    const int32_t n = int16_t(c.a.x);
    if (v == 0) m.divide_error(at);
    const int32_t q = n / int8_t(v), r = n % int8_t(v);
    if (q > 127 || q < -128) m.divide_error(at);
    c.a.l = uint8_t(q);
    c.a.h = uint8_t(r);
}
inline void idiv16(Machine& m, uint32_t at, uint16_t v) {
    Cpu& c = m.c;
    const int32_t n = int32_t((uint32_t(c.d.x) << 16) | c.a.x);
    if (v == 0) m.divide_error(at);
    const int64_t q = int64_t(n) / int16_t(v), r = int64_t(n) % int16_t(v);
    if (q > 32767 || q < -32768) m.divide_error(at);
    c.a.x = uint16_t(q);
    c.d.x = uint16_t(r);
}
inline void idiv32(Machine& m, uint32_t at, uint32_t v) {
    Cpu& c = m.c;
    const int64_t n = int64_t((uint64_t(c.d.e) << 32) | c.a.e);
    if (v == 0) m.divide_error(at);
    if (n == INT64_MIN && int32_t(v) == -1) m.divide_error(at);
    const int64_t q = n / int32_t(v), r = n % int32_t(v);
    if (q > INT32_MAX || q < INT32_MIN) m.divide_error(at);
    c.a.e = uint32_t(q);
    c.d.e = uint32_t(r);
}

// Bit scans. The destination keeps its value when the source is zero, as on Intel processors.
template <int B>
inline typename W<B>::T bsf(Cpu& c, uint64_t v, uint64_t old) {
    v &= W<B>::mask;
    c.zf = v == 0;
    if (v == 0) return typename W<B>::T(old);
    unsigned i = 0;
    while (!((v >> i) & 1)) i++;
    return typename W<B>::T(i);
}
template <int B>
inline typename W<B>::T bsr(Cpu& c, uint64_t v, uint64_t old) {
    v &= W<B>::mask;
    c.zf = v == 0;
    if (v == 0) return typename W<B>::T(old);
    unsigned i = B - 1;
    while (!((v >> i) & 1)) i--;
    return typename W<B>::T(i);
}

// String instructions with 32-bit addressing. src is the segment of the source operand; the destination is
// always ES:EDI. mode is 0 without a repeat prefix, 1 for REP or REPE and 2 for REPNE.
template <int B>
inline void movs(Machine& m, int src, int mode) {
    Cpu& c = m.c;
    constexpr int n = B / 8;
    const int step = c.df ? -n : n;
    if (mode == 0) {
        std::memmove(m.mem + uint32_t(c.base[ES] + c.di.e), m.mem + uint32_t(c.base[src] + c.si.e), n);
        c.si.e += step;
        c.di.e += step;
        return;
    }
    const uint32_t count = c.c.e;
    if (count == 0) return;
    const uint32_t s = c.base[src] + c.si.e, d = c.base[ES] + c.di.e;
    const uint64_t bytes = uint64_t(count) * n;
    const uint64_t gap = d > s ? uint64_t(d) - s : uint64_t(s) - d;
    if (!c.df && gap >= bytes && uint64_t(s) + bytes <= 0x100000000ull && uint64_t(d) + bytes <= 0x100000000ull) {
        std::memcpy(m.mem + d, m.mem + s, size_t(bytes));
        c.si.e += uint32_t(bytes);
        c.di.e += uint32_t(bytes);
        c.c.e = 0;
        return;
    }
    while (c.c.e != 0) {
        std::memmove(m.mem + uint32_t(c.base[ES] + c.di.e), m.mem + uint32_t(c.base[src] + c.si.e), n);
        c.si.e += step;
        c.di.e += step;
        c.c.e--;
    }
}

template <int B>
inline void stos(Machine& m, int mode) {
    Cpu& c = m.c;
    constexpr int n = B / 8;
    const int step = c.df ? -n : n;
    const uint32_t value = c.a.e;
    auto one = [&] {
        std::memcpy(m.mem + uint32_t(c.base[ES] + c.di.e), &value, n);
        c.di.e += step;
    };
    if (mode == 0) { one(); return; }
    if (B == 8 && !c.df && c.c.e != 0 && uint64_t(c.base[ES] + c.di.e) + c.c.e <= 0x100000000ull) {
        std::memset(m.mem + uint32_t(c.base[ES] + c.di.e), c.a.l, c.c.e);
        c.di.e += c.c.e;
        c.c.e = 0;
        return;
    }
    while (c.c.e != 0) { one(); c.c.e--; }
}

template <int B>
inline void lods(Machine& m, int src, int mode) {
    Cpu& c = m.c;
    constexpr int n = B / 8;
    const int step = c.df ? -n : n;
    auto one = [&] {
        typename W<B>::T v;
        std::memcpy(&v, m.mem + uint32_t(c.base[src] + c.si.e), n);
        if constexpr (B == 8) c.a.l = v; else if constexpr (B == 16) c.a.x = v; else c.a.e = v;
        c.si.e += step;
    };
    if (mode == 0) { one(); return; }
    while (c.c.e != 0) { one(); c.c.e--; }
}

template <int B>
inline void scas(Machine& m, int mode) {
    Cpu& c = m.c;
    constexpr int n = B / 8;
    const int step = c.df ? -n : n;
    auto one = [&] {
        typename W<B>::T v;
        std::memcpy(&v, m.mem + uint32_t(c.base[ES] + c.di.e), n);
        sub<B>(c, c.a.e, v, false);
        c.di.e += step;
    };
    if (mode == 0) { one(); return; }
    while (c.c.e != 0) {
        one();
        c.c.e--;
        if (mode == 1 ? !c.zf : c.zf) break;
    }
}

template <int B>
inline void cmps(Machine& m, int src, int mode) {
    Cpu& c = m.c;
    constexpr int n = B / 8;
    const int step = c.df ? -n : n;
    auto one = [&] {
        typename W<B>::T a, b;
        std::memcpy(&a, m.mem + uint32_t(c.base[src] + c.si.e), n);
        std::memcpy(&b, m.mem + uint32_t(c.base[ES] + c.di.e), n);
        sub<B>(c, a, b, false);
        c.si.e += step;
        c.di.e += step;
    };
    if (mode == 0) { one(); return; }
    while (c.c.e != 0) {
        one();
        c.c.e--;
        if (mode == 1 ? !c.zf : c.zf) break;
    }
}

template <int B>
inline void ins(Machine& m, int mode) {
    Cpu& c = m.c;
    constexpr int n = B / 8;
    const int step = c.df ? -n : n;
    auto one = [&] {
        const uint32_t v = m.port_in(c.d.x, n);
        std::memcpy(m.mem + uint32_t(c.base[ES] + c.di.e), &v, n);
        c.di.e += step;
    };
    if (mode == 0) { one(); return; }
    while (c.c.e != 0) { one(); c.c.e--; }
}

template <int B>
inline void outs(Machine& m, int src, int mode) {
    Cpu& c = m.c;
    constexpr int n = B / 8;
    const int step = c.df ? -n : n;
    auto one = [&] {
        uint32_t v = 0;
        std::memcpy(&v, m.mem + uint32_t(c.base[src] + c.si.e), n);
        m.port_out(c.d.x, v, n);
        c.si.e += step;
    };
    if (mode == 0) { one(); return; }
    while (c.c.e != 0) { one(); c.c.e--; }
}

inline void pushad(Machine& m) {
    Cpu& c = m.c;
    const uint32_t sp = c.sp.e;
    m.push32(c.a.e); m.push32(c.c.e); m.push32(c.d.e); m.push32(c.b.e);
    m.push32(sp); m.push32(c.bp.e); m.push32(c.si.e); m.push32(c.di.e);
}
inline void popad(Machine& m) {
    Cpu& c = m.c;
    c.di.e = m.pop32(); c.si.e = m.pop32(); c.bp.e = m.pop32(); (void)m.pop32();
    c.b.e = m.pop32(); c.d.e = m.pop32(); c.c.e = m.pop32(); c.a.e = m.pop32();
}
inline void pushaw(Machine& m) {
    Cpu& c = m.c;
    const uint16_t sp = c.sp.x;
    m.push16(c.a.x); m.push16(c.c.x); m.push16(c.d.x); m.push16(c.b.x);
    m.push16(sp); m.push16(c.bp.x); m.push16(c.si.x); m.push16(c.di.x);
}
inline void popaw(Machine& m) {
    Cpu& c = m.c;
    c.di.x = m.pop16(); c.si.x = m.pop16(); c.bp.x = m.pop16(); (void)m.pop16();
    c.b.x = m.pop16(); c.d.x = m.pop16(); c.c.x = m.pop16(); c.a.x = m.pop16();
}

// x87. Values are kept as doubles; 80-bit memory operands are converted on load and store.
inline double& st(Cpu& c, unsigned i) { return c.fpu.st[(c.fpu.top + i) & 7]; }
inline void fpush(Cpu& c, double v) {
    c.fpu.top = (c.fpu.top + 7) & 7;
    c.fpu.st[c.fpu.top] = v;
    c.fpu.valid |= uint8_t(1u << c.fpu.top);
}
inline double fpop(Cpu& c) {
    const double v = c.fpu.st[c.fpu.top];
    c.fpu.valid &= uint8_t(~(1u << c.fpu.top));
    c.fpu.top = (c.fpu.top + 1) & 7;
    return v;
}
inline uint16_t fstatus(const Cpu& c) { return uint16_t((c.fpu.sw & ~0x3800) | ((c.fpu.top & 7) << 11)); }
inline void finit(Cpu& c) { c.fpu = Fpu{}; }

// Sets C3, C2 and C0 as FCOM does: greater 000, less 001, equal 100, unordered 111.
inline void fcompare(Cpu& c, double a, double b) {
    uint16_t sw = c.fpu.sw & ~0x4500;
    if (std::isnan(a) || std::isnan(b)) sw |= 0x4500;
    else if (a < b) sw |= 0x0100;
    else if (a == b) sw |= 0x4000;
    c.fpu.sw = sw;
}

inline double fround(const Cpu& c, double v) {
    switch ((c.fpu.cw >> 10) & 3) {
    case 0: return std::nearbyint(v);  // the host rounds to nearest even by default
    case 1: return std::floor(v);
    case 2: return std::ceil(v);
    default: return std::trunc(v);
    }
}

template <typename T>
inline T fist(const Cpu& c, double v) {
    const double r = fround(c, v);
    constexpr T indefinite = T(T(1) << (sizeof(T) * 8 - 1));
    if (std::isnan(r) || r < double(std::numeric_limits<T>::min()) || r > double(std::numeric_limits<T>::max())) return indefinite;
    return T(r);
}

inline float f32(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }
inline uint32_t bits32(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
inline double f64(uint64_t bits) { double d; std::memcpy(&d, &bits, 8); return d; }
inline uint64_t bits64(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }

inline double read80(const Machine& m, uint32_t a) {
    const uint64_t mant = m.rd64(a);
    const uint16_t se = m.rd16(a + 8);
    const bool negative = (se & 0x8000) != 0;
    const int exp = se & 0x7FFF;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7FFF) v = (mant << 1) ? std::nan("") : INFINITY;
    else v = std::ldexp(double(mant), exp - 16383 - 63);
    return negative ? -v : v;
}

inline void write80(Machine& m, uint32_t a, double v) {
    uint16_t se = std::signbit(v) ? 0x8000 : 0;
    uint64_t mant = 0;
    v = std::fabs(v);
    if (std::isnan(v)) { se |= 0x7FFF; mant = 0xC000000000000000ull; }
    else if (std::isinf(v)) { se |= 0x7FFF; mant = 0x8000000000000000ull; }
    else if (v != 0.0) {
        int e;
        const double f = std::frexp(v, &e);  // v = f * 2^e with 0.5 <= f < 1
        mant = uint64_t(std::ldexp(f, 64));
        se |= uint16_t(e - 1 + 16383);
    }
    m.wr64(a, mant);
    m.wr16(a + 8, se);
}

// FBSTP: stores ST(0) as an 18-digit packed BCD integer.
inline void fbstp(Machine& m, uint32_t a) {
    Cpu& c = m.c;
    double v = fround(c, fpop(c));
    const bool negative = v < 0;
    uint64_t n = uint64_t(std::fabs(v));
    for (int i = 0; i < 9; i++) {
        const uint8_t lo = uint8_t(n % 10);
        n /= 10;
        const uint8_t hi = uint8_t(n % 10);
        n /= 10;
        m.wr8(a + i, uint8_t(lo | (hi << 4)));
    }
    m.wr8(a + 9, negative ? 0x80 : 0);
}

// FNSAVE and FRSTOR with the 32-bit protected-mode layout of 108 bytes.
inline void fnsave(Machine& m, uint32_t a) {
    Cpu& c = m.c;
    uint16_t tag = 0;
    for (unsigned i = 0; i < 8; i++)
        if (!(c.fpu.valid & (1u << i))) tag |= uint16_t(3u << (2 * i));
    m.wr32(a, c.fpu.cw);
    m.wr32(a + 4, fstatus(c));
    m.wr32(a + 8, tag);
    for (unsigned i = 12; i < 28; i += 4) m.wr32(a + i, 0);
    for (unsigned i = 0; i < 8; i++) write80(m, a + 28 + 10 * i, st(c, i));
    finit(c);
}
inline void frstor(Machine& m, uint32_t a) {
    Cpu& c = m.c;
    c.fpu.cw = m.rd16(a);
    const uint16_t sw = m.rd16(a + 4);
    c.fpu.top = (sw >> 11) & 7;
    c.fpu.sw = uint16_t(sw & ~0x3800);
    const uint16_t tag = m.rd16(a + 8);
    c.fpu.valid = 0;
    for (unsigned i = 0; i < 8; i++)
        if (((tag >> (2 * i)) & 3) != 3) c.fpu.valid |= uint8_t(1u << i);
    for (unsigned i = 0; i < 8; i++) st(c, i) = read80(m, a + 28 + 10 * i);
}

}  // namespace nax
