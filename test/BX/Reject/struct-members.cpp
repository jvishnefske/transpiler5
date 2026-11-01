// FR-257: the class features OUTSIDE the struct subset, each a located
// residual with its own wording (the census tags rejections by wording; the
// whole-class `class/struct definition` and whole-method `member function`
// wordings are kept for constructs bx still refuses without looking inside).
// A struct is in the subset only when it is an aggregate-like value type
// with a 1:1 Rust spelling: public, non-static, non-bit-field data members of
// Kernel types with no default member initializer, non-virtual member
// functions, at most one constructor whose body is a member-initializer list
// of every data member, and no destructor, base class, friend, nested type
// or operator overload. Each of those has a different meaning or no safe
// spelling in Rust (a destructor's timing, virtual dispatch, a bit-field's
// truncation, a default member initializer's implicit evaluation), so
// emitting any of them would be a silent behaviour change. A struct with
// such a member also gets ONE `struct with members outside the subset` at
// the class itself: its member functions cannot be emitted without it, and a
// per-function census attributes a class-located residual to every member.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}27{{$}}
#include <cstdio>
#include <string>

struct Base {
    int b;
};

// CHECK: struct-members.cpp:[[@LINE+1]]:1: error: unsupported: class with a base class (inheritance)
struct Derived : Base {
    int d;
};

// CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:1: error: unsupported: struct with members outside the subset
class Hidden {
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:9: error: unsupported: private or protected member
    int secret;

public:
    int open;
};

// CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:1: error: unsupported: struct with members outside the subset
struct Odd {
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:14: error: unsupported: bit-field
    unsigned flags : 3;
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:9: error: unsupported: default member initializer
    int init = 4;
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:16: error: unsupported: static data member
    static int shared;
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:17: error: unsupported: mutable data member
    mutable int cache;
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:15: error: unsupported: const data member
    const int fixed;
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:10: error: unsupported: data member of type 'int &'
    int& alias;
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:11: error: unsupported: data member of type 'float'
    float ratio;
};

// CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:1: error: unsupported: struct with members outside the subset
struct Poly {
    int v;
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:17: error: unsupported: virtual member function
    virtual int get() const;
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:9: error: unsupported: operator overload
    int operator+(const Poly& o) const;
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:5: error: unsupported: user-declared destructor
    ~Poly();
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:5: error: unsupported: user-declared copy or move constructor
    Poly(const Poly& o);
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:16: error: unsupported: friend declaration
    friend int peek(const Poly& p);
};

// CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:1: error: unsupported: struct with members outside the subset
struct Ctors {
    int a;
    int b;
    Ctors(int x) : a(x), b(x) {}
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:5: error: unsupported: overloaded constructor
    Ctors(int x, int y) : a(x), b(y) {}
};

struct Body {
    int a;
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:5: error: unsupported: constructor whose body is not a member-initializer list of every data member
    Body(int x) : a(0) { a = x; }
};

struct Partial {
    int a;
    int b;
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:5: error: unsupported: constructor whose body is not a member-initializer list of every data member
    Partial(int x) : a(x) {}
};

// CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:1: error: unsupported: struct with members outside the subset
struct Nest {
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:12: error: unsupported: nested class
    struct Inner {
        int z;
    };
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:10: error: unsupported: enum inside a class
    enum Kind { One };
    // CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:17: error: unsupported: member typedef
    typedef int Alias;
    int n;
};

// CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:1: error: unsupported: union
union U {
    int i;
    float f;
};

// CHECK-NEXT: struct-members.cpp:[[@LINE+1]]:1: error: unsupported: class template
template <typename T>
struct Box {
    T t;
};

int main() {
    std::printf("%d\n", 1);
    return 0;
}
