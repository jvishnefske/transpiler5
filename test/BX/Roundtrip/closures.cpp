// FR-252: closures and callable parameters, C++ -> canonical Rust under
// --check-roundtrip (laws 1-2 on BOTH sides), plus the canonical C++. Pins:
// by-copy captures `[k]` <=> `move |..|`, by-reference `[&acc]` <=> a
// non-move closure; the capture list is DERIVED from the body (first use
// order; an unused capture such as `k` / `acc` in `own` is dropped: normal
// form N23) and a closure with no capture is never `move` (N24); `mutable` is
// printed exactly when a by-copy closure modifies its copy, `let mut f`
// exactly when the closure modifies state or is passed to `F&`; `const auto`
// and a written `-> void` are spellings; every parameter type and non-void
// return type is explicit on both sides. `template <typename F> R g(const F&
// f, ..)` <=> `fn g(f: &impl Fn(..) -> R, ..)` and `F& f` <=> `&mut impl
// FnMut(..)` (any template parameter name is a spelling of `F`), called as
// `g(add, ..)` <=> `g(&add, ..)` / `g(&mut bump, ..)`. A by-copy capture of a
// std::string is C++'s copy and Rust's move (`s` is never mentioned after).
// RUN: %emitrust-bx --check-roundtrip %s | FileCheck %s
// RUN: %emitrust-bx --emit=cpp %s | FileCheck --check-prefix=CPP %s
// CHECK:      fn apply(f: &impl Fn(i32) -> i32, mut x: i32) -> i32 {
// CHECK-NEXT:     return f(x) + 1;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn each(g: &mut impl FnMut(i32), mut n: i32) {
// CHECK-NEXT:     for i in 0..n {
// CHECK-NEXT:         g(i);
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn shapes(mut k: i32, v: &mut Vec<i32>) -> i32 {
// CHECK-NEXT:     let mut acc: i32 = 0;
// CHECK-NEXT:     let add = move |mut a: i32| -> i32 {
// CHECK-NEXT:         return a + k;
// CHECK-NEXT:     };
// CHECK-NEXT:     let mut bump = |mut a: i32| {
// CHECK-NEXT:         acc += a;
// CHECK-NEXT:     };
// CHECK-NEXT:     let mut cnt = move || -> i32 {
// CHECK-NEXT:         k += 1;
// CHECK-NEXT:         return k;
// CHECK-NEXT:     };
// CHECK-NEXT:     let none = |a: i32| -> i32 {
// CHECK-NEXT:         return a * 2;
// CHECK-NEXT:     };
// CHECK-NEXT:     bump(add(1));
// CHECK-NEXT:     let mut c: i32 = cnt() + none(3);
// CHECK-NEXT:     each(&mut bump, 3);
// CHECK-NEXT:     let mut push = |mut x: i32| {
// CHECK-NEXT:         v.push(x + acc);
// CHECK-NEXT:     };
// CHECK-NEXT:     push(apply(&add, c));
// CHECK-NEXT:     let mut s: String = String::from("ab");
// CHECK-NEXT:     let own = move |mut i: usize| -> i8 {
// CHECK-NEXT:         return s.as_bytes()[i] as i8;
// CHECK-NEXT:     };
// CHECK-NEXT:     return acc + (own(1usize) as i32);
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn main() {
// CHECK-NEXT:     let mut v: Vec<i32> = vec![1];
// CHECK-NEXT:     let mut r: i32 = shapes(4, &mut v);
// CHECK-NEXT:     print!("{} {}\n", r, v.len());
// CHECK-NEXT: }
//
// CPP:      #include <cstdio>
// CPP-NEXT: #include <string>
// CPP-NEXT: #include <vector>
// CPP-EMPTY:
// CPP-NEXT: template <typename F> int apply(const F& f, int x) {
// CPP-NEXT:     return f(x) + 1;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: template <typename F> void each(F& g, int n) {
// CPP-NEXT:     for (int i = 0; i < n; ++i) {
// CPP-NEXT:         g(i);
// CPP-NEXT:     }
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int shapes(int k, std::vector<int>& v) {
// CPP-NEXT:     int acc = 0;
// CPP-NEXT:     auto add = [k](int a) -> int {
// CPP-NEXT:         return a + k;
// CPP-NEXT:     };
// CPP-NEXT:     auto bump = [&acc](int a) {
// CPP-NEXT:         acc += a;
// CPP-NEXT:     };
// CPP-NEXT:     auto cnt = [k]() mutable -> int {
// CPP-NEXT:         ++k;
// CPP-NEXT:         return k;
// CPP-NEXT:     };
// CPP-NEXT:     auto none = [](const int a) -> int {
// CPP-NEXT:         return a * 2;
// CPP-NEXT:     };
// CPP-NEXT:     bump(add(1));
// CPP-NEXT:     int c = cnt() + none(3);
// CPP-NEXT:     each(bump, 3);
// CPP-NEXT:     auto push = [&v, &acc](int x) {
// CPP-NEXT:         v.push_back(x + acc);
// CPP-NEXT:     };
// CPP-NEXT:     push(apply(add, c));
// CPP-NEXT:     std::string s = "ab";
// CPP-NEXT:     auto own = [s](std::size_t i) -> char {
// CPP-NEXT:         return s[i];
// CPP-NEXT:     };
// CPP-NEXT:     return acc + static_cast<int>(own(static_cast<std::size_t>(1)));
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int main() {
// CPP-NEXT:     std::vector<int> v = {1};
// CPP-NEXT:     int r = shapes(4, v);
// CPP-NEXT:     std::printf("%d %zu\n", r, v.size());
// CPP-NEXT:     return 0;
// CPP-NEXT: }
#include <cstdio>
#include <string>
#include <vector>

template <typename F>
int apply(const F& f, int x) {
    return f(x) + 1;
}

template <typename Fn>
void each(Fn& g, int n) {
    for (int i = 0; i < n; ++i) {
        g(i);
    }
}

int shapes(int k, std::vector<int>& v) {
    int acc = 0;
    auto add = [k](int a) -> int { return a + k; };
    auto bump = [&acc](int a) { acc = acc + a; };
    auto cnt = [k]() mutable -> int { k = k + 1; return k; };
    const auto none = [](const int a) -> int { return a * 2; };
    bump(add(1));
    int c = cnt() + none(3);
    each(bump, 3);
    auto push = [&acc, &v](int x) -> void {
        v.push_back(x + acc);
    };
    push(apply(add, c));
    std::string s = "ab";
    auto own = [k, s, acc](std::size_t i) -> char {
        return s[i];
    };
    return acc + static_cast<int>(own(1));
}

int main() {
    std::vector<int> v = {1};
    int r = shapes(4, v);
    std::printf("%d %zu\n", r, v.size());
    return 0;
}
