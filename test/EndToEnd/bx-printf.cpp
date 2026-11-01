// REQUIRES: cargo
// FR-250: emitrust-bx printf -> print!, end to end (byte-diff vs clang++
// and glibc). Pins the PROBED mapping on the values the probe covered:
// %.Nf rounding of exact ties (0.125 -> 0.12, 2.5 -> 2, 0.5 -> 0, 1.5 -> 2:
// glibc and Rust both round the exact decimal half-to-even), 1e300 in full,
// inf/-inf (produced at run time), -0.0, integer width with '-'/'0' flags
// including negatives ("-0042"), every length modifier, %c, %s with a
// literal, %% and literal braces (escaped {{ }} on the Rust side).
// NaN is deliberately absent: its SIGN out of 0.0/0.0 is unspecified and
// differs between run-time and constant-folded division, so a NaN byte-diff
// would test the compilers, not the mapping (bx::fmt_f's "nan"/"-nan" is
// unit-pinned instead).
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_printf > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>

double grow(double x, int times) {
    for (int i = 0; i < times; ++i) {
        x = x * 10.0;
    }
    return x;
}

int main() {
    const int i = -42;
    const long long l = -1234567890123LL;
    const unsigned u = 4294967295u;
    const unsigned long long ul = 18446744073709551615ull;
    const std::size_t z = 31337;
    std::printf("[%d][%5d][%-5d][%05d][%i][%3d]\n", i, i, i, i, i, 12345);
    std::printf("[%lld][%ld][%20lld][%-20lld]\n", l, l, l, l);
    std::printf("[%u][%12u][%llu][%025llu][%zu][%-8zu][%lu]\n", u, u, ul, ul, z, z, z);
    std::printf("[%.2f][%.1f][%.0f][%.0f][%.0f][%.3f]\n", 0.125, 0.25, 2.5, 0.5, 1.5, 0.0005);
    std::printf("[%f][%.10f][%.2f][%.2f]\n", 3.14159265358979, 1.0 / 3.0, -0.0, -2.675);
    std::printf("[%f]\n", 1e300);
    const double big = grow(1e300, 9);
    std::printf("[%f][%.2f][%.1f]\n", big, -big, big * 0.5);
    std::printf("[%c%c%c][%s][%s]\n", 'b', 'x', 65 + 25, "lit", "with {braces} and %");
    std::printf("100%% {not a placeholder} {{double}}\n");
    std::printf("tab\there \"quoted\" back\\slash\n");
    return 0;
}
