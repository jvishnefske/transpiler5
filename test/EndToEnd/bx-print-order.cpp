// REQUIRES: cargo
// FR-263: the ACCEPTED neighbours of the print-order refusal
// (BX/Reject/print-order.cpp), byte-diffed against clang++. Each statement
// here has ONE legal C++ output, so the subset keeps it: a printing call
// nested in another's arguments (arguments are sequenced before the callee's
// body), printing calls as the operands of `&&` and `?:` (sequenced), one
// printing call beside pure ones, two calls of a pure function, a template
// called twice in one expression with closures that do not print, and a
// printing closure called once per statement. g++ prints the same bytes (it
// was checked by hand; the lit oracle runs clang only).
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_print_order > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>

int g(int x) {
    std::printf("g%d ", x);
    return x;
}

int sq(int x) {
    return x * x;
}

int outer(int a, int b) {
    std::printf("outer%d,%d ", a, b);
    return a + b;
}

template <typename F>
int run(const F& f, int x) {
    return f(x) + 1;
}

int main() {
    int a = outer(g(1), 2);
    int b = g(sq(3) + sq(4));
    int c = 0;
    if (g(5) > 4 && g(6) > 9) {
        c = 1;
    }
    int d = g(7) > 0 ? g(8) : g(9);
    auto tri = [](int y) -> int {
        return y * 3;
    };
    auto dbl = [](int y) -> int {
        return y + y;
    };
    int f = run(tri, 4) + run(dbl, 5);
    auto say = [](int y) -> int {
        std::printf("say%d ", y);
        return y;
    };
    int h = say(10) + sq(2);
    std::printf("\n%d %d %d %d %d\n", a, b, c, d, f + h);
    return 0;
}
