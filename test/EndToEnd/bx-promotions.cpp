// REQUIRES: cargo
// FR-250: emitrust-bx C++ -> Rust, the usual-arithmetic-conversion traps,
// end to end (byte-diff vs clang++). Every one of these is an IMPLICIT C++
// conversion the Kernel must make explicit with the exact C++ semantics:
// a signed/unsigned comparison converts the int to unsigned (-1 < 1u is
// false), unsigned char + unsigned char promotes to int (no u8 wrap),
// int + unsigned long long converts to unsigned long long, a compound
// assignment computed in a WIDER type narrows back (`c += 100` on a char,
// `i *= 2.5` on an int), ++ on a char wraps through int, and bool <-> int
// both ways. Distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_promotions > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>

bool lt_mixed(int a, unsigned b) {
    return a < b;
}

int add_uc(unsigned char a, unsigned char b) {
    return a + b;
}

unsigned long long mix_wide(int a, unsigned long long b) {
    return a + b;
}

int char_compound(char c, int k) {
    c += k;
    char d = c;
    ++d;
    d++;
    return d;
}

int int_times_double(int i, double f) {
    i *= f;
    i -= 0.75;
    return i;
}

int bools(bool p, int q) {
    int r = p + p;
    bool s = q;
    bool t = !q;
    return r * 100 + s * 10 + t;
}

int main() {
    std::printf("lt %d %d %d\n", lt_mixed(-1, 1u), lt_mixed(3, 4u), lt_mixed(7, 2u));
    std::printf("uc %d %d\n", add_uc(200, 100), add_uc(1, 2));
    std::printf("wide %llu %llu\n", mix_wide(-5, 3ull), mix_wide(9, 18446744073709551000ull));
    std::printf("cc %d %d %d\n", char_compound('a', 100), char_compound(-100, -40), char_compound(120, 5));
    std::printf("id %d %d\n", int_times_double(7, 2.5), int_times_double(-3, 1.1));
    std::printf("bools %d %d %d\n", bools(true, 0), bools(false, 42), bools(true, -1));
    unsigned char uc = 255;
    ++uc;
    unsigned short_wrap = 65535u;
    short_wrap += 1u;
    std::printf("tail %d %u %d\n", uc, short_wrap, -1 < 0u);
    return 0;
}
