// FR-261: the pointer ALIAS shapes rustc rejects (measured on rustc 1.96 and
// 1.98) are LOCATED refusals, never left to rustc. Pins: R1, two arguments
// rooted in one variable when one is a mutable pointer (`copy(v.data() + 1,
// v.data(), 2)` is E0502, `two(&x, &x)` E0499, `f(from_mut(&mut x), x)`
// E0503), a mutable pointer argument whose root is also used elsewhere in the
// statement (C++ leaves the order unspecified), a pointer into the receiver
// passed to its own non-const member function; a mutable re-slice whose offset
// reads its own root (`&mut v[v.len()..]` is E0502, `p = &mut p[p[0]..]`
// E0503); R2, a mutable pointer local's root used while it is in scope
// (`int* q = p + 1; p[0] = 1; q[0] = 2;` is E0499/E0503) and a const pointer
// local's root modified while it is in scope (`v.push_back` while `w` points
// into v: E0502, and iterator invalidation in C++). And the two loop shapes a
// pointer would otherwise turn into SILENT miscompiles (C++ re-evaluates
// `i < bound` every iteration, Rust's range once): a counting loop whose body
// makes a mutable pointer to the vector behind `vv[0].size()` or to the bound
// `n`, and a range-for whose body makes a mutable pointer to the iterated
// vector.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}12{{$}}
#include <cstdio>
#include <vector>

struct Acc {
    int a;
    int b;
    void add(const int* p) { a += *p; }
    int get() const { return a; }
};

void copy(int* dst, const int* src, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
}

int bump(int* p) {
    *p += 1;
    return *p;
}

void two(int* p, int* q) {
    *p += *q;
}

void r1(std::vector<int>& v, int x) {
    // CHECK: pointer-alias.cpp:[[@LINE+1]]:5: error: unsupported: 'v' is passed to 'copy' as a mutable pointer and also used by another argument of the same call (aliasing: a second borrow in Rust)
    copy(v.data() + 1, v.data(), 2);
    // CHECK-NEXT: pointer-alias.cpp:[[@LINE+1]]:5: error: unsupported: 'x' is passed to 'two' as a mutable pointer and also used by another argument of the same call (aliasing: a second borrow in Rust)
    two(&x, &x);
    // CHECK-NEXT: pointer-alias.cpp:[[@LINE+1]]:10: error: unsupported: a mutable slice of 'v' starts at an offset that reads 'v' (a second borrow in Rust)
    bump(v.data() + (v.size() - 1));
    // CHECK-NEXT: pointer-alias.cpp:[[@LINE+1]]:13: error: unsupported: 'x' is passed to 'bump' as a mutable pointer and also used elsewhere in the statement (C++ leaves the order unspecified)
    int y = bump(&x) + x;
    Acc acc = {1, 2};
    // CHECK-NEXT: pointer-alias.cpp:[[@LINE+1]]:5: error: unsupported: 'acc' is passed by reference to 'add', a non-const member function called on it (aliasing)
    acc.add(&acc.b);
    std::printf("%d\n", y);
}

void r2(int* p, std::vector<int>& v, Acc* s, std::size_t* sp) {
    int* q = p + 1;
    // CHECK-NEXT: pointer-alias.cpp:[[@LINE+1]]:5: error: unsupported: 'p' is used while 'q', a mutable pointer derived from it, is in scope (a second borrow in Rust)
    p[0] = 1;
    q[0] = 2;
    const int* w = v.data();
    // CHECK-NEXT: pointer-alias.cpp:[[@LINE+1]]:5: error: unsupported: 'v' is modified or moved while 'w', a pointer into it, is in scope (a borrow conflict in Rust)
    v.push_back(3);
    std::printf("%d\n", w[0]);
    int* t = &s->a;
    // CHECK-NEXT: pointer-alias.cpp:[[@LINE+1]]:5: error: unsupported: 's' is used while 't', a mutable pointer derived from it, is in scope (a second borrow in Rust)
    s->b = 4;
    *t = 5;
    // CHECK-NEXT: pointer-alias.cpp:[[@LINE+1]]:5: error: unsupported: a mutable slice of 'sp' starts at an offset that reads 'sp' (a second borrow in Rust)
    sp += sp[0];
}

int bounds(std::vector<std::vector<int>>& vv, int n) {
    int t = 0;
    // CHECK-NEXT: pointer-alias.cpp:[[@LINE+1]]:5: error: unsupported: counting loop body modifies or shadows 'vv', whose size is the bound
    for (std::size_t i = 0; i < vv[0].size(); ++i) {
        std::vector<std::vector<int>>* pv = &vv;
        (*pv)[0].push_back(7);
        t += 1;
    }
    // CHECK-NEXT: pointer-alias.cpp:[[@LINE+1]]:5: error: unsupported: counting loop body assigns or shadows the bound 'n'
    for (int i = 0; i < n; ++i) {
        int* pn = &n;
        *pn = 2;
        t += 1;
    }
    std::vector<int> v = {1, 2};
    // CHECK-NEXT: pointer-alias.cpp:[[@LINE+1]]:5: error: unsupported: the body of a range-for over 'v' modifies 'v' (iterator invalidation in C++, a borrow conflict in Rust)
    for (int x : v) {
        int* q = v.data();
        *q = x;
    }
    return t;
}

int main() {
    std::vector<int> v = {1, 2, 3};
    r1(v, 4);
    int k = 5;
    Acc acc = {6, 7};
    std::size_t z = 0;
    r2(&k, v, &acc, &z);
    std::vector<std::vector<int>> vv = {{8}};
    std::printf("%d\n", bounds(vv, 9));
    return 0;
}
