// REQUIRES: cargo
// FR-259: emitrust-bx C++ -> Rust, the <cstdint>/<cstddef> names in EVERY
// position end to end (byte-diff vs clang++; normal form N20): return types
// (`std::uint32_t`, `std::int16_t`), reference pointees (`const
// std::vector<std::uint8_t>&`, `std::vector<std::vector<std::uint16_t>>&`),
// template arguments, locals, `std::int64_t` / `std::uint64_t` / `size_t` /
// `std::ptrdiff_t`; and the 16-bit types (`short` <=> i16, `unsigned short`
// <=> u16) whose arithmetic is C++'s int promotion made explicit: a u16 sum
// that wraps only when narrowed back, a u16 product computed in int (kept
// below INT_MAX), an i16 narrowing of a negative int, a u32 FNV-style hash
// that wraps, a uint64_t that wraps. UB-free; distinct values at every call
// site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_typedefs > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

std::uint32_t Fnv(const std::vector<std::uint8_t>& data) {
    std::uint32_t h = 2166136261U;
    for (std::size_t i = 0; i < data.size(); ++i) {
        h = (h ^ data[i]) * 16777619U;
    }
    return h;
}

std::uint16_t AddWrap(std::uint16_t a, std::uint16_t b) {
    int wide = a + b;
    std::uint16_t narrow = a + b;
    return static_cast<std::uint16_t>(narrow + (wide > 65535 ? 1 : 0));
}

int MulPromoted(std::uint16_t a, std::uint16_t b) {
    return a * b;
}

std::int16_t Narrow(int x) {
    return static_cast<std::int16_t>(x);
}

std::uint64_t Mix64(std::uint64_t x, std::int64_t y) {
    std::uint64_t r = x * 6364136223846793005ULL + static_cast<std::uint64_t>(y);
    return r ^ (r >> 29);
}

std::int64_t Grid(std::vector<std::vector<std::uint16_t>>& g, std::ptrdiff_t bias) {
    std::int64_t s = bias;
    for (std::size_t r = 0; r < g.size(); ++r) {
        for (std::size_t c = 0; c < g[r].size(); ++c) {
            g[r][c] = static_cast<std::uint16_t>(g[r][c] + 1);
            s = s * 7 + g[r][c];
        }
    }
    return s;
}

int main() {
    std::vector<std::uint8_t> d = {104, 105, 255, 0, 33};
    std::printf("%u %u\n", Fnv(d), Fnv(std::vector<std::uint8_t>{1, 2, 3}));
    std::printf("%d %d %d\n", AddWrap(65530, 9), AddWrap(1000, 2345), MulPromoted(181, 179));
    std::printf("%d %d %d\n", Narrow(-40000), Narrow(70001), Narrow(-7));
    std::printf("%llu %llu\n", static_cast<unsigned long long>(Mix64(12345ULL, -3)),
                static_cast<unsigned long long>(Mix64(18446744073709551615ULL, 99)));
    std::vector<std::vector<std::uint16_t>> g = {{65535, 2}, {300, 41, 7}};
    std::int64_t gs = Grid(g, -5);
    std::printf("%lld %d %d\n", static_cast<long long>(gs), g[0][0], g[1][2]);
    return 0;
}
