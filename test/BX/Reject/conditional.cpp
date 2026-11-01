// FR-259: `c ? a : b` is in the subset as a scalar VALUE (<=> `if c { a }
// else { b }`). Every neighbour is a LOCATED refusal: a `?:` used as an
// lvalue -- assigned, incremented, its address taken (it would be one of two
// objects); a `?:` of class type (std::string, a struct, a std::vector: the
// copy or move it implies has no single Kernel term); a `?:` of pointer type
// (it would be a slice of one of two roots); GNU's `x ?: y`; and a `?:`
// evaluated only for its effect (a statement, not a value).
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}9{{$}}
#include <cstdio>
#include <string>
#include <vector>

struct P {
    int x;
};

int show(int v) {
    std::printf("%d\n", v);
    return v;
}

int f(int c, int x, int y, std::string a, std::string b, P p, P q, std::vector<int>& v) {
    // CHECK:      conditional.cpp:[[@LINE+1]]:6: error: unsupported: conditional operator `?:` used as an lvalue
    (c ? x : y) = 5;
    // CHECK-NEXT: conditional.cpp:[[@LINE+1]]:8: error: unsupported: conditional operator `?:` used as an lvalue
    ++(c ? x : y);
    // CHECK-NEXT: conditional.cpp:[[@LINE+1]]:16: error: unsupported: conditional operator `?:` used as an lvalue
    int* r = &(c ? x : y);
    // CHECK-NEXT: conditional.cpp:[[@LINE+1]]:21: error: unsupported: conditional operator `?:` of type 'std::string' (the subset's `?:` yields a scalar)
    std::string s = c ? a : b;
    // CHECK-NEXT: conditional.cpp:[[@LINE+1]]:11: error: unsupported: conditional operator `?:` of type 'P' (the subset's `?:` yields a scalar)
    P t = c ? p : q;
    // CHECK-NEXT: conditional.cpp:[[@LINE+1]]:20: error: unsupported: conditional operator `?:` of type 'int *' (the subset's `?:` yields a scalar)
    const int* w = c ? &x : &y;
    // CHECK-NEXT: conditional.cpp:[[@LINE+1]]:13: error: unsupported: conditional operator `?:`
    int z = x ?: y;
    // CHECK-NEXT: conditional.cpp:[[@LINE+1]]:5: error: unsupported: conditional operator `?:`
    c ? show(1) : show(2);
    // CHECK-NEXT: conditional.cpp:[[@LINE+1]]:26: error: unsupported: conditional operator `?:` of type 'std::vector<int>' (the subset's `?:` yields a scalar)
    std::vector<int> u = c ? v : std::vector<int>{};
    return *r + z + static_cast<int>(s.size()) + t.x + *w + static_cast<int>(u.size());
}

int main() {
    return 0;
}
