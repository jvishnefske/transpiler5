// REQUIRES: cargo
// FR-260: emitrust-bx C++ -> Rust, anonymous namespaces and cross-namespace
// recursion end to end (byte-diff vs clang++). An anonymous namespace at the
// root is a plain root function (normal form N14: only linkage changes); one
// inside `parity` is a private Rust `fn`, called bare from its own namespace
// and as `crate::parity::step` from the nested `parity::check`. `even` and
// `odd` live in SIBLING namespaces and recurse into each other, so the C++
// printer must re-derive a prototype inside its namespace. Distinct values
// per call site; recursion depth is bounded; UB-free.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_ns_anon > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>

namespace {

unsigned int mix(unsigned int h, unsigned int x) {
    return h * 31u + x;
}

}  // namespace

namespace odds {
bool odd(int n);
}

namespace evens {

bool even(int n) {
    if (n == 0) {
        return true;
    }
    return odds::odd(n - 1);
}

}  // namespace evens

namespace odds {

bool odd(int n) {
    if (n == 0) {
        return false;
    }
    return evens::even(n - 1);
}

}  // namespace odds

namespace parity {

namespace {

int step(int n) {
    return n / 2;
}

}  // namespace

int halvings(int n) {
    int k = 0;
    while (n > 1) {
        n = step(n);
        k += 1;
    }
    return k;
}

namespace check {

int both(int n) {
    return parity::step(n) * 10 + halvings(n);
}

}  // namespace check

}  // namespace parity

int main() {
    unsigned int h = 7u;
    for (int i = 0; i < 5; ++i) {
        h = mix(h, static_cast<unsigned int>(i * 3 + 1));
    }
    std::printf("%u\n", h);
    std::printf("%d %d %d %d\n", evens::even(10), evens::even(7), odds::odd(9), odds::odd(4));
    std::printf("%d %d %d\n", parity::halvings(1000), parity::check::both(77), parity::check::both(6));
    return 0;
}
