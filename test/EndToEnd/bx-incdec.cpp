// REQUIRES: cargo
// FR-259: emitrust-bx C++ -> Rust, increments and decrements USED AS VALUES
// end to end (byte-diff vs clang++). `x++` <=> `{ x += 1; x - 1 }`, `++x`
// <=> `{ x += 1; x }`, `x--` / `--x` likewise, and for an unsigned variable
// the wrapping spellings: a byte-buffer writer `out[pos++] = b` through a
// `std::size_t&` cursor (written from scratch after woff2's Store* helpers),
// a reader `v[pos++]`, `while (n--)` / `while (n-- > 0)` countdowns, a
// `size_t` decremented through 0 (post- and pre-, the wrap is the value
// observed), a `long long` and an `unsigned` counter, an increment as a call
// argument and inside a `?:` arm. Each statement mentions the incremented
// variable once (the subset refuses `i++ + i`). UB-free; distinct values at
// every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_incdec > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

void StoreU16(std::uint32_t val, std::size_t& offset, std::vector<std::uint8_t>& dst) {
    dst[offset++] = static_cast<std::uint8_t>(val >> 8);
    dst[offset++] = static_cast<std::uint8_t>(val & 255U);
}

std::uint32_t ReadU16(const std::vector<std::uint8_t>& src, std::size_t& offset) {
    std::uint32_t hi = src[offset++];
    std::uint32_t lo = src[offset++];
    return (hi << 8) | lo;
}

int countdown(int n) {
    int s = 0;
    while (n--) {
        s = s * 3 + n;
    }
    int k = 4;
    while (k-- > 1) {
        s += 100;
    }
    return s + k;
}

long long counters(long long start, unsigned u) {
    long long a = start++;
    long long b = ++start;
    long long c = start--;
    unsigned d = u--;
    unsigned e = --u;
    return a * 10000 + b * 1000 + c * 100 + static_cast<long long>(d % 97U) + static_cast<long long>(e % 89U);
}

std::size_t wrap(std::size_t z) {
    std::size_t before = z--;
    std::size_t after = --z;
    return before + after % 1000;
}

int twice(int x) {
    return x * 2;
}

int args(int n, bool c) {
    int i = n;
    int r = twice(i++);
    int j = n;
    int q = c ? ++j : -n;
    return r + i * 7 + q * 11 + j;
}

int main() {
    std::vector<std::uint8_t> buf = {0, 0, 0, 0, 0, 0};
    std::size_t w = 0;
    StoreU16(4660U, w, buf);
    StoreU16(65021U, w, buf);
    std::size_t r = 0;
    std::uint32_t x = ReadU16(buf, r);
    std::uint32_t y = ReadU16(buf, r);
    std::printf("%u %u %d %d\n", x, y, static_cast<int>(w), static_cast<int>(r));
    std::printf("%d %d\n", countdown(5), countdown(0));
    std::printf("%lld %lld\n", counters(7LL, 3U), counters(-12LL, 0U));
    std::printf("%d %d\n", static_cast<int>(wrap(0) % 100000), static_cast<int>(wrap(9)));
    std::printf("%d %d\n", args(5, true), args(8, false));
    return 0;
}
