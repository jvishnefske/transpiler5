// REQUIRES: cargo
// FR-252: emitrust-bx C++ -> Rust, callable PARAMETERS end to end (byte-diff
// vs clang++). `template <typename F> R g(const F& f, ..)` <=> `fn g(f: &impl
// Fn(..) -> R, ..)` and `F& f` <=> `&mut impl FnMut(..)`; a call `g(add, 5)`
// <=> `g(&add, 5)` / `g(&mut bump, 5)`. Taking the closure BY REFERENCE is
// what makes the row exact: a `mutable` lambda's state advanced inside `g` is
// the caller's state afterwards in both languages (a by-value `F f` would
// copy it in C++ and is refused). Callable parameters passed on to another
// template, a template called before its definition (a prototype), one in a
// namespace, and one closure passed to two templates. UB-free; distinct
// values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_closure_template > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>
#include <vector>

template <typename F>
int fold(const F& f, const std::vector<int>& v, int init) {
    int acc = init;
    for (int x : v) {
        acc = f(acc, x);
    }
    return acc;
}

template <typename F>
void repeat(F& f, int times) {
    for (int i = 0; i < times; ++i) {
        f(i);
    }
}

template <typename F>
int twice_more(F& f, int base) {
    repeat(f, 2);
    return base + 1;
}

namespace ops {
template <typename F>
long long apply_ll(const F& f, long long x) {
    return f(x) + f(x + 1LL);
}
}  // namespace ops

int pipeline(int seed) {
    int bias = seed % 7;
    auto weigh = [bias](int acc, int x) -> int {
        return acc * 3 + x + bias;
    };
    std::vector<int> v = {4, 1, 9, 2};
    int r1 = fold(weigh, v, 1);
    int r2 = fold(weigh, v, 2);
    std::printf("fold: %d %d\n", r1, r2);
    return r1 - r2;
}

int counters(int seed) {
    int seen = 0;
    auto note = [&seen](int i) {
        seen = seen * 10 + i + 1;
    };
    repeat(note, 3);
    repeat(note, 2);
    int k = seed;
    auto tick = [k](int i) mutable {
        k = k + i * 100;
        std::printf("tick: k=%d\n", k);
    };
    repeat(tick, 3);
    int more = twice_more(tick, seed);
    tick(7);
    std::printf("counters: seen=%d k=%d more=%d\n", seen, k, more);
    return seen;
}

long long scaled(long long m) {
    auto mul = [m](long long x) -> long long {
        return x * m;
    };
    long long a = ops::apply_ll(mul, 5LL);
    long long b = ops::apply_ll(mul, 50LL);
    std::printf("scaled: %lld %lld\n", a, b);
    return a + b;
}

int main() {
    int p = pipeline(23);
    int c = counters(5);
    long long s = scaled(3LL);
    std::printf("main: %d %d %lld\n", p, c, s);
    return 0;
}
