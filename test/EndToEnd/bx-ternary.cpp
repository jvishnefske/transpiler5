// REQUIRES: cargo
// FR-259: emitrust-bx C++ -> Rust, the conditional operator end to end
// (byte-diff vs clang++). `c ? a : b` <=> `if c { a } else { b }`: arms of
// MIXED types under the usual arithmetic conversions (int/unsigned with a
// negative arm, char/int, int/double, long long/unsigned), a nested `?:` <=>
// `else if`, a `?:` as a condition, an index, a call argument (also for a
// `const int&` parameter), a printf argument and a counting-loop start; and
// LAZINESS: each arm calls a function that bumps its own counter through a
// reference, so evaluating the wrong arm (or both) changes the output.
// UB-free; distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_ternary > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>
#include <vector>

int bump(int& counter, int v) {
    counter += 1;
    return v * 3;
}

long long widen(bool c, int i, unsigned u) {
    unsigned w = c ? -1 : u;
    long long r = c ? i : u;
    return r + static_cast<long long>(w % 1000U);
}

int chars(bool c, char a, int k) {
    int q = c ? a : 300;
    char d = k > 2 ? a : 'z';
    return q + d;
}

double mix(bool c, int i, double d) {
    return c ? d : i;
}

int classify(int x) {
    return x < 0 ? -1 : x == 0 ? 0 : x < 10 ? 1 : 2;
}

int lazy(bool c, int x) {
    int left = 0;
    int right = 0;
    int other = 0;
    int a = c ? bump(left, x) : bump(right, x + 1);
    int b = !c ? bump(other, a) : a > 20 ? 7 : 8;
    return a + b + left * 1000 + right * 100 + other * 10;
}

int peek(const int& v, int k) {
    return v + k;
}

int positions(const std::vector<int>& v, bool first, int n) {
    int s = 0;
    if (first ? v[0] > 3 : v[1] > 3) {
        s += 100;
    }
    s += v[first ? 0 : 2];
    s += peek(n > 5 ? v[1] : n, first ? 1 : 2);
    for (int i = first ? 1 : 0; i < 3; ++i) {
        s += v[static_cast<std::size_t>(i)];
    }
    return s;
}

int main() {
    std::printf("%lld %lld\n", widen(true, -8, 11U), widen(false, 13, 4000000123U));
    std::printf("%d %d\n", chars(true, 'A', 3), chars(false, 'B', 1));
    std::printf("%.3f %.3f\n", mix(true, 4, 2.25), mix(false, -9, 7.5));
    std::printf("%d %d %d %d\n", classify(-5), classify(0), classify(6), classify(42));
    std::printf("%d %d %d\n", lazy(true, 9), lazy(false, 2), lazy(true, 4));
    std::vector<int> v = {2, 5, 11};
    std::printf("%d %d\n", positions(v, true, 7), positions(v, false, 3));
    int t = 17;
    std::printf("%c %d\n", t > 10 ? 'y' : 'n', t % 2 == 0 ? t / 2 : 3 * t + 1);
    return 0;
}
