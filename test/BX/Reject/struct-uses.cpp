// FR-257: uses of structs OUTSIDE the subset, each located at the use. Pins:
// moving out of a data member (Rust would partially move the object), a
// struct variable without an initializer (indeterminate scalars), `S()`
// value-initialization and an aggregate initializer that omits members (both
// zero-fill; the subset spells every member), member access and member calls
// on a temporary, a static member function called through an object, a
// pointer to a struct (FR-261), and the receiver-alias shapes rustc 1.98
// rejects (measured): `p.take(p)` with `take(const P&)` on a `&mut self`
// method (E0502), `p.set(bump(p))` (E0499 -- also an unspecified C++ order),
// `v[0].set(v[0].get())` / `v[0].set(v.size())` (IndexMut is not a two-phase
// borrow, E0502), and a statement that both modifies `p` through a call and
// reads it elsewhere (C++ leaves the order unspecified).
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}17{{$}}
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

struct P {
    int x;
    std::string s;
    int get() const { return x; }
    void set(int k) { x = k; }
    void take(const P& o) { x += o.x; }
    static int zero() { return 0; }
};

struct Q {
    int a;
    Q(int v) : a(v) {}
};

struct Pair {
    int a;
    int b;
};

P make() {
    P p = {1, "m"};
    return p;
}

int bump(P& p) {
    p.x += 1;
    return p.x;
}

int main() {
    P p = {2, "p"};
    // CHECK: struct-uses.cpp:[[@LINE+1]]:21: error: unsupported: std::move of something other than a variable
    std::string t = std::move(p.s);
    // CHECK-NEXT: struct-uses.cpp:[[@LINE+1]]:5: error: unsupported: struct variable without an initializer (default-initialized data members)
    P d;
    // CHECK-NEXT: struct-uses.cpp:[[@LINE+1]]:14: error: unsupported: value-initialized or default-initialized struct (`S()`, `S s;`)
    Pair z = Pair();
    // CHECK-NEXT: struct-uses.cpp:[[@LINE+1]]:17: error: unsupported: aggregate initialization that omits data members
    Pair half = {1};
    // CHECK-NEXT: struct-uses.cpp:[[@LINE+1]]:13: error: unsupported: member access on a temporary
    int y = make().x;
    // CHECK-NEXT: struct-uses.cpp:[[@LINE+1]]:13: error: unsupported: member function call on a temporary
    int w = make().get();
    // CHECK-NEXT: struct-uses.cpp:[[@LINE+1]]:14: error: unsupported: static member function called through an object
    int s0 = p.zero();
    // CHECK-NEXT: struct-uses.cpp:[[@LINE+1]]:5: error: unsupported: variable of type 'P *'
    P* ptr = &p;
    // CHECK-NEXT: struct-uses.cpp:[[@LINE+1]]:5: error: unsupported: 'p' is passed by reference to 'take', a non-const member function called on it (aliasing)
    p.take(p);
    // CHECK-NEXT: struct-uses.cpp:[[@LINE+2]]:5: error: unsupported: an argument of 'set' modifies 'p', the object it is called on (a second borrow in Rust)
    // CHECK-NEXT: struct-uses.cpp:[[@LINE+1]]:11: error: unsupported: 'p' is passed to 'bump' by non-const reference and also used elsewhere in the statement (C++ leaves the order unspecified)
    p.set(bump(p));
    // CHECK-NEXT: struct-uses.cpp:[[@LINE+1]]:23: error: unsupported: 'p' is passed to 'bump' by non-const reference and also used elsewhere in the statement (C++ leaves the order unspecified)
    int u = p.get() + bump(p);
    std::vector<P> v = {p};
    // CHECK-NEXT: struct-uses.cpp:[[@LINE+1]]:5: error: unsupported: non-const member function 'set' is called on an element of 'v' with an argument that reads 'v' (a second borrow in Rust)
    v[0].set(v[0].get());
    // CHECK-NEXT: struct-uses.cpp:[[@LINE+1]]:5: error: unsupported: non-const member function 'set' is called on an element of 'v' with an argument that reads 'v' (a second borrow in Rust)
    v[0].set(static_cast<int>(v.size()));
    // CHECK-NEXT: struct-uses.cpp:[[@LINE+1]]:13: error: unsupported: 'p' is passed to 'bump' by non-const reference and also used elsewhere in the statement (C++ leaves the order unspecified)
    int k = bump(p) + p.x;
    // CHECK-NEXT: struct-uses.cpp:[[@LINE+1]]:5: error: unsupported: expression statement without effect
    Q(3);
    // CHECK-NEXT: struct-uses.cpp:[[@LINE+1]]:77: error: unsupported: member access through a pointer
    std::printf("%d %d %d %d %d %d\n", y, w, s0, u, k, z.a + half.a + d.x + ptr->x);
    std::printf("%s\n", t.c_str());
    return 0;
}
