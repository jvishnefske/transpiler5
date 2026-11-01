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

// FR-257 moved this pin forward: an aggregate `struct` is in the subset; a
// union (one storage for several members) is its item-level neighbour.
// CHECK-NEXT: multi.cpp:[[@LINE+1]]:1: error: unsupported: union
union Point {
    int x;
};

// FR-259 moved three pins forward: `auto b = a;` (normal form N22), `int c =
// a++;` and `a > 0 ? c : a` are in the subset; their refused neighbours are an
// `auto&` (a reference local), an increment beside another use of its
// variable (unsequenced in C++) and a `?:` used as an lvalue.
int f(int a) {
    // CHECK-NEXT: multi.cpp:[[@LINE+1]]:5: error: unsupported: variable of type 'int &'
    auto& b = a;
    // CHECK-NEXT: multi.cpp:[[@LINE+1]]:5: error: unsupported: switch statement
    switch (a) { default: break; }
    // CHECK-NEXT: multi.cpp:[[@LINE+1]]:13: error: unsupported: increment/decrement of 'a' used as a value in a statement that also uses 'a' (C++ leaves the order unsequenced or unspecified)
    int c = a++ + a;
    // CHECK-NEXT: multi.cpp:[[@LINE+1]]:5: error: unsupported: `for` loop outside the canonical counting form `for (int i = a; i < b; ++i)`
    for (int i = 10; i > 0; --i) {}
    // CHECK-NEXT: multi.cpp:[[@LINE+1]]:16: error: unsupported: conditional operator `?:` used as an lvalue
    int* q = &(a > 0 ? c : a);
    return a + *q;
}

int main() {
    // CHECK-NEXT: multi.cpp:[[@LINE+1]]:5: error: unsupported: printf conversion '%x'
    std::printf("%x\n", f(3));
    return 0;
}
