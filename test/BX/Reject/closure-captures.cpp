// FR-252: the lambda shapes outside the closure Kernel are LOCATED refusals,
// and the ones a mechanical rewrite would bring inside NAME it (FR-254's
// normalizer): a default capture `[=]` / `[&]` / `[=, &x]` lists the
// explicit captures it stands for, read off the closure record's fields
// (`[k, &acc]`); mixed by-copy and by-reference captures (Rust spells one
// mode per closure: `move` or not); an init-capture; `this` / `*this`; a
// generic lambda (an `auto` parameter); a lambda inside a lambda; a copy of a
// closure (`auto g = f;` copies its state in C++, Rust would move it); a
// lambda outside `auto f = [..](..) {..};` (an argument, a temporary called
// in place); a closure over a non-scalar parameter or result; a by-copy
// capture of a reference (C++ copies the referent, `move` the reference) or
// of a pointer; a closure used inside another closure; a callable parameter
// taken BY VALUE `F f` (a copy of a mutable closure's state in C++, a borrow
// in Rust) or by `F&&`, a template with two type parameters, one never
// instantiated, and one instantiated with closures of different signatures.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}20{{$}}
#include <cstdio>
#include <vector>

// CHECK:      closure-captures.cpp:[[@LINE+1]]:1: error: unsupported: callable parameter taken by value `F f` (normalizer: take it as `const F& f`, or `F& f` for a closure that modifies state -- C++ copies a by-value closure and its state, Rust would borrow it)
template <typename F>
int byval(F f, int x) {
    return f(x);
}

// CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:1: error: unsupported: parameter of type 'F &&' in a function template
template <typename F>
int fwd(F&& f, int x) {
    return f(x);
}

// CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:1: error: unsupported: function template (the subset's templates take exactly one type parameter, a callable)
template <typename F, typename G>
int both(const F& f, const G& g) {
    return f(1) + g(2);
}

// CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:1: error: unsupported: function template that is never instantiated (its callable's signature is unknown)
template <typename F>
int never(const F& f) {
    return f(0);
}

// CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:1: error: unsupported: function template instantiated with closures of different signatures (Rust's `impl Fn` names one)
template <typename F>
int mixed_sigs(const F& f) {
    return static_cast<int>(f(1));
}

struct Box2 {
    int v;
    int get() const;
};

int Box2::get() const {
    // CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:22: error: unsupported: capture of `this` (a closure in the subset captures local variables only)
    auto self_read = [this]() -> int {
        return v;
    };
    return self_read();
}

int defaults(int k, int& acc) {
    // CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:14: error: unsupported: default capture `[=]` (normalizer: list the captures explicitly: `[k]`)
    auto a = [=](int x) -> int {
        return x * k;
    };
    // CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:14: error: unsupported: default capture `[&]` (normalizer: list the captures explicitly: `[&acc, &k]`)
    auto b = [&](int x) {
        acc = acc + x + k;
    };
    // CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:14: error: unsupported: default capture `[=, &acc]` (normalizer: list the captures explicitly: `[&acc, k]`)
    auto c = [=, &acc](int x) {
        acc = acc + x * k;
    };
    // CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:14: error: unsupported: mixed by-copy and by-reference captures `[k, &acc]` (normalizer: copy each by-copy capture into a local before the lambda and capture everything by reference)
    auto d = [k, &acc](int x) {
        acc = acc + k + x;
    };
    // CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:14: error: unsupported: init-capture `z = k + 1` (normalizer: declare it as a local before the lambda and capture that)
    auto e = [z = k + 1](int x) -> int {
        return x + z;
    };
    // CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:14: error: unsupported: generic lambda (an `auto` parameter)
    auto g = [](auto x) {
        return x;
    };
    return k;
}

int shapes(int k, std::vector<int>& v) {
    auto outer = [k](int x) -> int {
        // CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:9: error: unsupported: lambda inside a lambda body (nested closures are not in the subset)
        auto inner = [x]() -> int {
            return x;
        };
        return inner() + k;
    };
    auto f = [k]() mutable -> int {
        k = k + 1;
        return k;
    };
    // CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:5: error: unsupported: copy of a closure (`auto g = f;` copies its state in C++, Rust would move it; the subset only calls a closure)
    auto f2 = f;
    // CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:13: error: unsupported: lambda expression
    int t = [k](int x) -> int { return x + k; }(3);
    // CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:20: error: unsupported: closure returning 'std::vector<int>' (the subset's closures return a scalar or nothing)
    auto vec_ret = [k]() -> std::vector<int> {
        return {k};
    };
    // CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:42: error: unsupported: closure parameter of type 'std::vector<int>' (the subset's closures take scalars)
    auto vec_param = [](std::vector<int> w) -> int {
        return 0;
    };
    // CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:5: error: unsupported: closure 'copy_ref' captures the reference 'v' by copy (C++ copies the referent, a Rust `move` closure the reference)
    auto copy_ref = [v]() -> std::size_t {
        return v.size();
    };
    auto add = [k](int x) -> int { return x + k; };
    // CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:5: error: unsupported: closure 'uses' uses the callable 'add' (a closure is not captured by another closure in the subset)
    auto uses = [&add](int x) -> int {
        return add(x);
    };
    auto narrow = [](int x) -> long long { return x; };
    int r = byval(add, 1) + fwd(add, 2) + both(add, add) + mixed_sigs(add) + mixed_sigs(narrow);
    return r + t;
}

int pointer(int k) {
    int* p = &k;
    // CHECK-NEXT: closure-captures.cpp:[[@LINE+1]]:5: error: unsupported: closure 'ptr' captures the pointer 'p'
    auto ptr = [p]() -> int {
        return p[0];
    };
    return ptr();
}

int main() {
    return 0;
}
