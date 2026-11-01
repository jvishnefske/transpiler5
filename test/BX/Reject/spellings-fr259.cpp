// FR-259: the spelling normal forms N20-N22 accept a <cstdint> name, a
// `static` / `inline` free function and an `auto` declaration; their
// neighbours stay LOCATED refusals. A TU that declares `uint8_t` as
// something else (here `int`, inside a namespace) turns the <cstdint>
// resolution OFF -- a reference pointee spelled `const uint8_t &` is then
// refused rather than guessed; a `static` (internal) function of namespace n
// called from outside it (Rust: a private `fn`, E0603); `extern` and
// `constexpr` functions; an `auto` that deduces a reference, an iterator, a
// lambda or an initializer_list; `auto z{3}` (list-initialization) and
// `decltype(auto)`; a `static` local. `for (auto&& y : v)` over an lvalue
// vector deduces `int&` and IS accepted (a mutable range-for reference).
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}12{{$}}
#include <cstdio>
#include <vector>

namespace a {
// CHECK:      spellings-fr259.cpp:[[@LINE+1]]:1: error: unsupported: typedef
typedef int uint8_t;
// CHECK-NEXT: spellings-fr259.cpp:[[@LINE+1]]:22: error: unsupported: parameter of type 'const uint8_t &'
int f(const uint8_t& x) {
    return x;
}
}  // namespace a

namespace n {
static int q(int x) {
    return x - 1;
}
}  // namespace n

int r(int x) {
    // CHECK-NEXT: spellings-fr259.cpp:[[@LINE+1]]:12: error: unsupported: 'n::q' is internal to namespace 'n' (a C++ anonymous-namespace member, a private Rust `fn`) and is referenced from outside it
    return n::q(x);
}

// CHECK-NEXT: spellings-fr259.cpp:[[@LINE+1]]:12: error: unsupported: extern function
extern int e(int x) {
    return x;
}

// CHECK-NEXT: spellings-fr259.cpp:[[@LINE+1]]:15: error: unsupported: constexpr function
constexpr int k(int x) {
    return x;
}

int g(std::vector<int>& v, int x) {
    // CHECK-NEXT: spellings-fr259.cpp:[[@LINE+1]]:5: error: unsupported: variable of type 'int &'
    auto& rx = x;
    // CHECK-NEXT: spellings-fr259.cpp:[[@LINE+1]]:5: error: unsupported: variable of type 'iterator'
    auto it = v.begin();
    // CHECK-NEXT: spellings-fr259.cpp:[[@LINE+1]]:5: error: unsupported: auto-typed declaration of a lambda
    auto lam = [](int q) { return q; };
    // CHECK-NEXT: spellings-fr259.cpp:[[@LINE+1]]:5: error: unsupported: variable of type 'std::initializer_list<int>'
    auto il = {1, 2};
    // CHECK-NEXT: spellings-fr259.cpp:[[@LINE+1]]:5: error: unsupported: list-initialization (the subset spells it `T x = e;`)
    auto z{3};
    // CHECK-NEXT: spellings-fr259.cpp:[[@LINE+1]]:5: error: unsupported: decltype-typed declaration
    decltype(auto) d = x;
    for (auto&& y : v) {
        x += y;
    }
    // CHECK-NEXT: spellings-fr259.cpp:[[@LINE+1]]:5: error: unsupported: static local variable
    static auto s = 4;
    return x;
}

int main() {
    return 0;
}
