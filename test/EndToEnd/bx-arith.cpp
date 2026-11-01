// REQUIRES: cargo
// FR-250: emitrust-bx C++ -> Rust, signed arithmetic, end to end. The real
// oracle for the scalar rows: the emitted crate's stdout is byte-diffed
// against a clang++ build of this same file. Pins: C++ truncating `/` and
// `%` on negative operands (== Rust), shifts and bitwise ops with the
// shift amount in a different integer type, every implicit conversion as
// an explicit `as` (int -> long long widening, char narrowing that wraps
// modulo 256 -- char is SIGNED, so 200 prints -56 -- double -> int
// truncation toward zero), and bool -> int. Every call site passes
// DISTINCT values so a wiring bug (argument to the wrong parameter) shows.
// The subset has no `main(argc, ...)`, so inputs are literals at distinct
// call sites; neither side folds across the transpiler. UB-free: no signed
// overflow, every shift in range, no division by zero.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_arith > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>

int poly(int x, int a, int b, int c) {
    return (a * x + b) * x + c;
}

long long divmods(long long n, long long d) {
    return (n / d) * 1000 + n % d;
}

int shifts(int v, long long s) {
    return (v << s) ^ (v >> (s + 1));
}

int bits(int a, int b) {
    return (a & b) | (a ^ ~b);
}

char narrow(int v) {
    return static_cast<char>(v);
}

int trunc_toward_zero(double d) {
    return static_cast<int>(d);
}

long long widen_sum(int a, char c, unsigned char uc, bool b) {
    long long acc = a;
    acc = acc * 100000 + c;
    acc = acc + uc + b;
    return acc;
}

int main() {
    std::printf("poly %d %d %d\n", poly(3, 2, -5, 7), poly(-4, 1, 9, -2), poly(11, -3, 4, 13));
    std::printf("divmod %lld %lld %lld %lld\n", divmods(-17, 5), divmods(17, -5), divmods(-17, -5),
                divmods(123456789012LL, 997));
    std::printf("shift %d %d %d\n", shifts(13, 2), shifts(1000, 5), shifts(77, 0));
    std::printf("bits %d %d\n", bits(23130, 4080), bits(-9, 300));
    std::printf("narrow %d %d %d %d\n", narrow(200), narrow(-300), narrow(65), narrow(383));
    std::printf("trunc %d %d %d\n", trunc_toward_zero(-3.99), trunc_toward_zero(7.5), trunc_toward_zero(-0.25));
    std::printf("widen %lld %lld\n", widen_sum(-12, 'x', 250, true), widen_sum(40000, -7, 3, false));
    int t = 0;
    int k = 1;
    while (k < 30) {
        t += k * k - k / 3;
        k += 2;
    }
    std::printf("loop %d %d\n", t, -t % 7);
    return 0;
}
