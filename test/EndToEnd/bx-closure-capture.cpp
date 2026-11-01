// REQUIRES: cargo
// FR-252: emitrust-bx C++ -> Rust, closures end to end (byte-diff vs
// clang++). By-copy captures <=> `move` closures and by-reference captures
// <=> non-move closures. The load-bearing rows: a variable modified AFTER a
// by-copy closure is made and BEFORE it is called (the closure must see the
// OLD value), a `mutable` lambda's private state (it advances while the outer
// variable stays put), a by-reference closure that modifies its capture (the
// outer variable sees every call once the closure is dead), closures made
// inside a loop over the loop variable, closures with no capture, and the
// implicit conversions of arguments and results (explicit casts in Rust).
// UB-free; distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_closure_capture > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>

int copy_then_modify(int seed) {
    int k = seed;
    auto snap = [k](int a) -> int {
        return a * 10 + k;
    };
    k = k + 7;
    int first = snap(1);
    k = k * 3;
    int second = snap(2);
    std::printf("copy: k=%d first=%d second=%d\n", k, first, second);
    return first + second + k;
}

int ref_counter(int start) {
    int hits = start;
    auto hit = [&hits](int by) {
        hits += by;
    };
    hit(3);
    hit(40);
    hit(500);
    std::printf("ref: hits=%d\n", hits);
    return hits;
}

int ref_step(int start) {
    int k = start;
    auto step = [&k]() -> int {
        k = k * 2 + 1;
        return k;
    };
    int a = step();
    int b = step();
    int c = step();
    std::printf("step: %d %d %d k=%d\n", a, b, c, k);
    return k;
}

int private_state(int seed) {
    int n = seed;
    auto next = [n]() mutable -> int {
        n = n + 5;
        return n;
    };
    int a = next();
    int b = next();
    n = 1000;
    int c = next();
    std::printf("state: %d %d %d n=%d\n", a, b, c, n);
    return a + b + c;
}

long long widen(int x) {
    auto conv = [](long long v, double w) -> long long {
        return v * 3 + static_cast<long long>(w);
    };
    long long r = conv(x, 2.75);
    char c = 'q';
    auto shout = [c](int up) -> char {
        return static_cast<char>(c - up);
    };
    std::printf("widen: %lld %c %c\n", r, shout(32), shout(1));
    return r;
}

int loop_captures(int n) {
    int total = 0;
    for (int i = 0; i < n; ++i) {
        auto sq = [i](int off) -> int {
            return i * i + off;
        };
        auto add = [&total](int v) {
            total += v;
        };
        add(sq(i + 100));
    }
    std::printf("loop: total=%d\n", total);
    return total;
}

bool parity(unsigned x) {
    unsigned mask = 1U;
    auto odd = [mask](unsigned v) -> bool {
        return (v & mask) != 0U;
    };
    auto wrap = [](unsigned v) -> unsigned {
        return v - 3U;
    };
    std::printf("parity: %d %d %u\n", odd(x) ? 1 : 0, odd(x + 1U) ? 1 : 0, wrap(1U));
    return odd(x);
}

double area(double w, double h) {
    const double scale = 0.5;
    auto tri = [w, h, scale]() -> double {
        return w * h * scale;
    };
    double t = tri();
    std::printf("area: %.3f\n", t);
    return t;
}

int main() {
    int a = copy_then_modify(4);
    int b = ref_counter(9);
    int c = ref_step(2);
    int d = private_state(11);
    long long e = widen(17);
    int f = loop_captures(5);
    bool g = parity(41U);
    double h = area(3.5, 4.0);
    std::printf("main: %d %d %d %d %lld %d %d %.2f\n", a, b, c, d, e, f, g ? 1 : 0, h);
    return 0;
}
