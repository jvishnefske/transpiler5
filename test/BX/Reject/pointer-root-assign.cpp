// FR-261 R2: while a MUTABLE pointer local is in scope its root may not be
// mentioned at all -- including as the TARGET of an assignment. The Kernel
// holds an assignment target as a name, not a Var read, and the first R2
// check only looked for reads, so `int* p = &x; x = 5; *p` was accepted and
// emitted Rust that rustc rejects (E0506): silently unbuildable. Every
// mutating form of the root (`=`, `op=`, `++`, a field store) is pinned.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// COUNT: {{^}}4{{$}}
#include <cstdio>

struct S { int a; };

void assign() {
    int x = 1;
    int* p = &x;
    // CHECK: pointer-root-assign.cpp:[[@LINE+1]]:5: error: unsupported: 'x' is used while 'p', a mutable pointer derived from it, is in scope (a second borrow in Rust)
    x = 5;
    std::printf("%d\n", *p);
}

void compound() {
    int x = 1;
    int* p = &x;
    // CHECK: pointer-root-assign.cpp:[[@LINE+1]]:5: error: unsupported: 'x' is used while 'p', a mutable pointer derived from it, is in scope (a second borrow in Rust)
    x += 5;
    std::printf("%d\n", *p);
}

void incr() {
    int x = 1;
    int* p = &x;
    // CHECK: pointer-root-assign.cpp:[[@LINE+1]]:5: error: unsupported: 'x' is used while 'p', a mutable pointer derived from it, is in scope (a second borrow in Rust)
    ++x;
    std::printf("%d\n", *p);
}

void field() {
    S s = {1};
    S* p = &s;
    // CHECK: pointer-root-assign.cpp:[[@LINE+1]]:5: error: unsupported: 's' is used while 'p', a mutable pointer derived from it, is in scope (a second borrow in Rust)
    s.a = 4;
    std::printf("%d\n", p->a);
}

int main() {
    assign();
    compound();
    incr();
    field();
    return 0;
}
