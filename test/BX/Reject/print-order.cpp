// FR-263: C++ leaves the order of unsequenced operands unspecified -- call
// arguments, printf arguments, the operands of `+`, `-`, `<`, ... -- and Rust
// fixes it left to right. Two calls that may PRINT, unsequenced against each
// other, therefore have two legal C++ outputs: g++ prints `g6 g5` where clang
// and rustc print `g5 g6`. The byte-diff oracle runs clang only and cannot see
// it, so the subset refuses the statement, located at the second call. "May
// print" is a whole-program fixpoint: a printf, a call of a printing function
// or member function, a local closure whose body prints, a callable parameter
// some call site binds to a printing closure. The accepted neighbours (nested
// calls, `&&`/`?:` operands, one printing call beside pure ones) are pinned
// byte-for-byte in EndToEnd/bx-print-order.cpp.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// COUNT: {{^}}6{{$}}
#include <cstdio>

int g(int x) {
    std::printf("g%d ", x);
    return x;
}

int twice(int x) {
    return g(x) * 2;
}

struct Log {
    int n;
    int note(int x) const {
        std::printf("note%d ", x);
        return x + n;
    }
};

template <typename F>
int run(const F& f, int x) {
    return f(x) + 1;
}

void cases() {
    // CHECK: print-order.cpp:[[@LINE+1]]:34: error: unsupported: two calls that may print in one statement without a sequence point between them (C++ leaves their order unspecified; normalizer: evaluate each into a local first)
    std::printf("%d %d\n", g(5), g(6));
    // CHECK: print-order.cpp:[[@LINE+1]]:20: error: unsupported: two calls that may print
    int d = g(3) - twice(4);
    Log lg = Log{1};
    // CHECK: print-order.cpp:[[@LINE+1]]:26: error: unsupported: two calls that may print
    int e = lg.note(1) + lg.note(2);
    auto shout = [](int x) -> int {
        std::printf("shout%d ", x);
        return x;
    };
    // CHECK: print-order.cpp:[[@LINE+1]]:24: error: unsupported: two calls that may print
    int h = shout(1) + shout(2);
    // CHECK: print-order.cpp:[[@LINE+1]]:29: error: unsupported: two calls that may print
    int k = run(shout, 1) + run(shout, 2);
    // CHECK: print-order.cpp:[[@LINE+1]]:20: error: unsupported: two calls that may print
    int m = g(7) < shout(8);
    std::printf("%d %d %d %d %d\n", d, e, h, k, m);
}

int main() {
    cases();
    return 0;
}
