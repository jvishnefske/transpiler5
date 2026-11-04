// FR-266: every USE of a rejected struct in an otherwise-accepted function is
// a located residual AT THE USE, `use of a rejected struct 'S'`: a parameter,
// local or return of the struct's type (by value, by reference, or as a
// std::vector element), a member function call (member or static), a data
// member read or write, and an aggregate construction. A struct holding a
// rejected struct by value (directly or in a std::vector) is rejected in
// turn, at the data member and at the class. Before this the users carried
// no residual of their own, so a per-function census (bx-census /
// bx-frontier) counted `byref`, `write`, `make`, `many` and `outer` as clean
// EMIT -- phantom functions that can never be emitted. The rejected struct's
// OWN members (`Bad::get`, `Bad::zero`) keep their class-level attribution,
// and a function that never names a rejected struct (`clean`, `main`) gets
// nothing.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}19{{$}}
#include <cstdio>
#include <vector>
// CHECK: rejected-struct-uses.cpp:[[@LINE+1]]:1: error: unsupported: struct with members outside the subset
struct Bad {
    int x;
    // CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+1]]:17: error: unsupported: mutable data member
    mutable int m;
    int get() const { return x; }
    static int zero() { return 0; }
};
// CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+1]]:1: error: unsupported: struct with members outside the subset
struct Outer {
    // CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+1]]:9: error: unsupported: use of a rejected struct 'Bad'
    Bad b;
    int y;
};
// CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+1]]:1: error: unsupported: struct with members outside the subset
struct Holder {
    // CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+1]]:22: error: unsupported: use of a rejected struct 'Bad'
    std::vector<Bad> v;
};
// CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+1]]:22: error: unsupported: use of a rejected struct 'Bad'
int byref(const Bad& b) {
    // CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+2]]:12: error: unsupported: use of a rejected struct 'Bad'
    // CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+1]]:22: error: unsupported: use of a rejected struct 'Bad'
    return b.get() + b.x;
}
// CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+1]]:16: error: unsupported: use of a rejected struct 'Bad'
int write(Bad& b, int k) {
    // CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+1]]:5: error: unsupported: use of a rejected struct 'Bad'
    b.x = k;
    // CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+1]]:12: error: unsupported: use of a rejected struct 'Bad'
    return Bad::zero();
}
// CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+1]]:5: error: unsupported: use of a rejected struct 'Bad'
Bad make(int k) {
    // (the aggregate's own residual is kept: a use residual is ADDITIVE, it
    // never hides a blocker the expression already had)
    // CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+3]]:5: error: unsupported: use of a rejected struct 'Bad'
    // CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+2]]:13: error: unsupported: aggregate initialization of a struct with rejected data members
    // CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+1]]:13: error: unsupported: use of a rejected struct 'Bad'
    Bad b = {k, k};
    return b;
}
// CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+1]]:34: error: unsupported: use of a rejected struct 'Bad'
int many(const std::vector<Bad>& v) {
    return 0;
}
// CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+1]]:24: error: unsupported: use of a rejected struct 'Outer'
int outer(const Outer& o) {
    // CHECK-NEXT: rejected-struct-uses.cpp:[[@LINE+1]]:12: error: unsupported: use of a rejected struct 'Outer'
    return o.y;
}
// CHECK-NOT: error
int clean(int k) {
    return k + 1;
}
int main() {
    std::printf("%d\n", clean(1));
    return 0;
}
