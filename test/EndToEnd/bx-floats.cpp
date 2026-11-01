// REQUIRES: cargo
// FR-250: emitrust-bx C++ -> Rust, double arithmetic and conversions, end
// to end (byte-diff vs clang++). Pins: f64 + - * / and comparisons,
// int -> double and double -> int (truncation, in range), long long ->
// double, unary minus, a Newton iteration and a series whose last printed
// digits expose any reassociation or precision change, and %.Nf output.
// Distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_floats > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>

double newton_sqrt(double v, int iters) {
    double x = v;
    for (int i = 0; i < iters; ++i) {
        x = 0.5 * (x + v / x);
    }
    return x;
}

double leibniz(int terms) {
    double acc = 0.0;
    double sign = 1.0;
    for (int k = 0; k < terms; ++k) {
        acc += sign / (2 * k + 1);
        sign = -sign;
    }
    return 4.0 * acc;
}

long long to_cents(double dollars) {
    return static_cast<long long>(dollars * 100.0);
}

double mean3(int a, long long b, double c) {
    return (a + b + c) / 3;
}

int main() {
    std::printf("sqrt %.12f %.12f %.6f\n", newton_sqrt(2.0, 6), newton_sqrt(10.0, 8), newton_sqrt(1e6, 30));
    std::printf("pi %.10f %.15f\n", leibniz(1000), leibniz(12345));
    std::printf("cents %lld %lld %lld\n", to_cents(19.99), to_cents(-0.07), to_cents(123456.789));
    std::printf("mean %f %f\n", mean3(1, 2, 3.5), mean3(-7, 9000000000LL, 0.125));
    double x = 1.0;
    int n = 0;
    while (x > 1e-9) {
        x = x / 3.0;
        n++;
    }
    bool small = x < 1e-9;
    std::printf("third %d %.20f %d\n", n, x, small);
    return 0;
}
