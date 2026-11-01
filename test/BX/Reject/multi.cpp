// FR-250 / FR-254 seam: cxx_import does NOT stop at the first unsupported
// construct. Every one is a located `error: unsupported:` line on stderr,
// sorted by location, exit 1 -- here eight distinct residuals across items,
// declarations, statements and expressions, plus the exact count so a
// regression to first-failure (or a duplicate report) fails.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}8{{$}}
#include <cstdio>

// CHECK: multi.cpp:[[@LINE+1]]:1: error: unsupported: global variable
int counter = 0;

// CHECK-NEXT: multi.cpp:[[@LINE+1]]:1: error: unsupported: class/struct definition
struct Point {
    int x;
};

int f(int a) {
    // CHECK-NEXT: multi.cpp:[[@LINE+1]]:5: error: unsupported: auto-typed declaration
    auto b = a;
    // CHECK-NEXT: multi.cpp:[[@LINE+1]]:5: error: unsupported: switch statement
    switch (a) { default: break; }
    // CHECK-NEXT: multi.cpp:[[@LINE+1]]:13: error: unsupported: increment/decrement used as a value
    int c = a++;
    // CHECK-NEXT: multi.cpp:[[@LINE+1]]:5: error: unsupported: `for` loop outside the canonical counting form `for (int i = a; i < b; ++i)`
    for (int i = 10; i > 0; --i) {}
    // CHECK-NEXT: multi.cpp:[[@LINE+1]]:12: error: unsupported: conditional operator `?:`
    return a > 0 ? c : a;
}

int main() {
    // CHECK-NEXT: multi.cpp:[[@LINE+1]]:5: error: unsupported: printf conversion '%x'
    std::printf("%x\n", f(3));
    return 0;
}
