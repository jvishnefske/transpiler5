// REQUIRES: cargo
// FR-250: emitrust-bx C++ -> Rust, control flow, end to end (byte-diff vs
// clang++). Pins: recursion and MUTUAL recursion (C++ prototypes <=> Rust
// order-free fns), else-if chains, while with break/continue, nested
// canonical counting loops with literal AND variable bounds (`for i in a..b`
// evaluates the bound once; the subset only admits loops whose bound the
// body cannot change, so that agrees with C++ re-evaluating it), early
// `return;` from a void function, and `int main() { ... return 0; }`.
// Distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_control > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>

bool is_odd(int n);

bool is_even(int n) {
    if (n == 0) {
        return true;
    }
    return is_odd(n - 1);
}

bool is_odd(int n) {
    if (n == 0) {
        return false;
    }
    return is_even(n - 1);
}

long long gcd(long long a, long long b) {
    if (b == 0) {
        return a;
    }
    return gcd(b, a % b);
}

int collatz_steps(long long n) {
    int steps = 0;
    while (n != 1) {
        if (n % 2 == 0) {
            n = n / 2;
        } else {
            n = 3 * n + 1;
        }
        ++steps;
    }
    return steps;
}

int grade(int score) {
    if (score >= 90) {
        return 4;
    } else if (score >= 80) {
        return 3;
    } else if (score >= 70) {
        return 2;
    } else if (score >= 60) {
        return 1;
    }
    return 0;
}

void report(int lo, int hi) {
    if (lo > hi) {
        std::printf("empty\n");
        return;
    }
    int sum = 0;
    for (int i = lo; i < hi; ++i) {
        if (i % 4 == 0) {
            continue;
        }
        if (i > 40) {
            break;
        }
        sum += i;
    }
    std::printf("report %d %d %d\n", lo, hi, sum);
}

int main() {
    std::printf("parity %d %d %d\n", is_even(10), is_odd(7), is_even(13));
    std::printf("gcd %lld %lld\n", gcd(1071, 462), gcd(17179869184LL, 393216));
    std::printf("collatz %d %d\n", collatz_steps(27), collatz_steps(97));
    std::printf("grade %d %d %d %d %d\n", grade(95), grade(85), grade(72), grade(61), grade(12));
    report(3, 17);
    report(25, 60);
    report(9, 2);
    const int n = 5;
    int pairs = 0;
    for (int a = 0; a < n; ++a) {
        for (int b = a; b < 7; ++b) {
            if ((a + b) % 3 == 0) {
                pairs = pairs + a * 10 + b;
            }
        }
    }
    std::printf("pairs %d\n", pairs);
    return 0;
}
