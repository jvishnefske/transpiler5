// FR-250: the out-of-subset C++ constructs are named IN C++ TERMS, located,
// and never silently emitted: each of these must be a Residual (a wrong
// lowering of any would be a silent miscompile, e.g. `x = y = 3` or a
// non-canonical `for` whose bound C++ re-evaluates every iteration). A
// printf argument residual is located at the offending ARGUMENT.
// RUN: not %emitrust-bx %s 2>&1 | FileCheck %s
#include <cstdio>

double half(double x) { return x / 2.0; }

void g(int n, unsigned long long w, std::size_t z) {
    int x = 0;
    // CHECK: cxx-constructs.cpp:[[@LINE+1]]:9: error: unsupported: assignment used as a value
    x = n = 3;
    // CHECK: cxx-constructs.cpp:[[@LINE+1]]:5: error: unsupported: do-while loop
    do { x = x + 1; } while (x < n);
    // CHECK: cxx-constructs.cpp:[[@LINE+1]]:5: error: unsupported: variable of type 'float'
    float f = 1.5f;
    // CHECK: cxx-constructs.cpp:[[@LINE+1]]:5: error: unsupported: declaration without an initializer
    int u;
    // CHECK: cxx-constructs.cpp:[[@LINE+1]]:5: error: unsupported: list-initialization (the subset spells it `T x = e;`)
    int v{4};
    // CHECK: cxx-constructs.cpp:[[@LINE+1]]:25: error: unsupported: printf %s of something other than a string literal
    std::printf("%s\n", n > 0 ? "a" : "b");
    // CHECK: cxx-constructs.cpp:[[@LINE+1]]:27: error: unsupported: printf argument of type 'int' for '%lld'
    std::printf("%lld\n", n);
    // CHECK: cxx-constructs.cpp:[[@LINE+1]]:5: error: unsupported: printf width/flags on '%8.2f'
    std::printf("%8.2f\n", half(3.0));
    // CHECK: cxx-constructs.cpp:[[@LINE+1]]:20: error: unsupported: call to library function 'puts'
    const int pr = puts("x");
    // CHECK: cxx-constructs.cpp:[[@LINE+1]]:5: error: unsupported: counting loop body assigns the loop variable 'i'
    for (int i = 0; i < 3; ++i) { i = 5; }
    // CHECK: cxx-constructs.cpp:[[@LINE+1]]:5: error: unsupported: `for` loop outside the canonical counting form `for (int i = a; i < b; ++i)`
    for (int i = 0; i < n; i = i + 1) {}
    // CHECK: cxx-constructs.cpp:[[@LINE+1]]:5: error: unsupported: identifier 'match' is a Rust keyword
    int match = 1;
}

int main() {
    g(1, 2ull, 3);
    return 0;
}
