// REQUIRES: cargo
// FR-250: emitrust-bx C++ -> Rust, unsigned rows, end to end (byte-diff vs
// clang++). C++ unsigned arithmetic WRAPS by definition; the canonical Rust
// is .wrapping_add/sub/mul/neg (plain `+` would panic in a debug build and
// the subset refuses it). Pins: 32- and 64-bit wraparound in add/sub/mul,
// unary minus on unsigned, `~`, unsigned shifts, `/` and `%` (which cannot
// wrap and stay plain), size_t <=> usize, unsigned char promotion to int,
// and the `x op= e` <=> `x = x.wrapping_op(e)` compound form. Distinct
// values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_unsigned > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>

unsigned fnv1a_step(unsigned h, unsigned char b) {
    h ^= b;
    h *= 16777619u;
    return h;
}

unsigned long long splitmix(unsigned long long x) {
    unsigned long long z = x + 11400714819323198485ull;
    z = (z ^ (z >> 30)) * 13787848793156543929ull;
    z = (z ^ (z >> 27)) * 10723151780598845931ull;
    return z ^ (z >> 31);
}

std::size_t tri(std::size_t n) {
    std::size_t s = 0;
    for (std::size_t i = 0; i < n; ++i) {
        s += i;
    }
    return s;
}

unsigned rotl(unsigned v, unsigned r) {
    return (v << r) | (v >> (32u - r));
}

int promote(unsigned char a, unsigned char b) {
    return a - b;
}

int main() {
    unsigned h = 2166136261u;
    for (int i = 0; i < 6; ++i) {
        h = fnv1a_step(h, static_cast<unsigned char>(i * 37 + 11));
    }
    std::printf("fnv %u\n", h);
    std::printf("mix %llu %llu\n", splitmix(1ull), splitmix(18446744073709551615ull));
    unsigned u = 3u;
    u -= 10u;
    unsigned neg = -u;
    std::printf("wrap %u %u %u %u\n", u, neg, ~neg, u * 4000000000u);
    std::printf("tri %zu %zu\n", tri(10), tri(1000));
    std::printf("rotl %u %u\n", rotl(2882400001u, 4u), rotl(305419896u, 31u));
    std::printf("divmod %u %u %llu\n", 4000000000u / 7u, 4000000000u % 7u, 18446744073709551615ull / 3ull);
    std::printf("promote %d %d\n", promote(10, 250), promote(255, 1));
    std::size_t z = 0;
    --z;
    std::printf("size %zu %zu\n", z, z / 2);
    return 0;
}
