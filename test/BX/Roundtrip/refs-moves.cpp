// FR-251: references, range-for and copy vs move, C++ -> canonical Rust
// under --check-roundtrip (laws 1-2 on BOTH sides), plus the canonical C++.
// Pins `int&` / `const long long&` <=> `&mut i32` / `&i64` (`*x` in value
// and assignment position in Rust, `x += ..` <=> `*x += ..`), a temporary
// bound to `const T&` (`bump(k, 10)` <=> `bump(&mut k, &10i64)`); the four
// range-for forms: `for (int x : v)` <=> `for mut x in v.iter().copied()`,
// `for (const int x : v)` <=> `for x in v.iter().copied()`,
// `for (const int& x : v)` <=> `for x in v.iter()`, `for (int& x : v)` <=>
// `for x in v.iter_mut()`, and over strings `.iter()` / `.iter().cloned()`;
// COPY vs MOVE read from clang's AST: a copy-constructed local, by-value
// argument or `push_back(x)` <=> `.clone()`, `std::move(x)` <=> `x`,
// `return v;` of a by-value parameter (implicit move) <=> `return v;`,
// `return v;` of a `const T&` parameter (a copy) <=> `return v.clone();`.
// RUN: %emitrust-bx --check-roundtrip %s | FileCheck %s
// RUN: %emitrust-bx --emit=cpp %s | FileCheck --check-prefix=CPP %s
// CHECK:      fn bump(x: &mut i32, by: &i64) {
// CHECK-NEXT:     *x += (*by) as i32;
// CHECK-NEXT:     *x *= 2;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn walk(v: &mut Vec<i32>, names: &Vec<String>) -> i64 {
// CHECK-NEXT:     let mut s: i64 = 0i64;
// CHECK-NEXT:     for mut x in v.iter().copied() {
// CHECK-NEXT:         s += x as i64;
// CHECK-NEXT:     }
// CHECK-NEXT:     for x in v.iter().copied() {
// CHECK-NEXT:         s += x as i64;
// CHECK-NEXT:     }
// CHECK-NEXT:     for x in v.iter() {
// CHECK-NEXT:         s += (*x) as i64;
// CHECK-NEXT:     }
// CHECK-NEXT:     for x in v.iter_mut() {
// CHECK-NEXT:         *x += 1;
// CHECK-NEXT:     }
// CHECK-NEXT:     for w in names.iter() {
// CHECK-NEXT:         s += w.len() as i64;
// CHECK-NEXT:     }
// CHECK-NEXT:     for mut w in names.iter().cloned() {
// CHECK-NEXT:         w.push_str("!");
// CHECK-NEXT:         s += w.len() as i64;
// CHECK-NEXT:     }
// CHECK-NEXT:     return s;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn keep(mut v: Vec<i32>) -> Vec<i32> {
// CHECK-NEXT:     v.push(0);
// CHECK-NEXT:     return v;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn copy_of(v: &Vec<i32>) -> Vec<i32> {
// CHECK-NEXT:     return v.clone();
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn main() {
// CHECK-NEXT:     let mut k: i32 = 3;
// CHECK-NEXT:     bump(&mut k, &10i64);
// CHECK-NEXT:     let mut v: Vec<i32> = vec![1, 2];
// CHECK-NEXT:     let mut names: Vec<String> = vec![String::from("a"), String::from("bb")];
// CHECK-NEXT:     let mut w: i64 = walk(&mut v, &names);
// CHECK-NEXT:     let mut c: Vec<i32> = v.clone();
// CHECK-NEXT:     let mut m: Vec<i32> = c;
// CHECK-NEXT:     let mut kept: Vec<i32> = keep(v.clone());
// CHECK-NEXT:     let mut moved: Vec<i32> = keep(m);
// CHECK-NEXT:     let mut all: Vec<Vec<i32>> = Vec::new();
// CHECK-NEXT:     all.push(kept.clone());
// CHECK-NEXT:     all.push(moved);
// CHECK-NEXT:     let mut s: String = String::from("x");
// CHECK-NEXT:     names.push(s.clone());
// CHECK-NEXT:     names.push(s);
// CHECK-NEXT:     print!("{} {} {} {} {}\n", k, w, all.len(), copy_of(&v).len(), names.len());
// CHECK-NEXT: }
// CPP:      #include <cstdio>
// CPP-NEXT: #include <string>
// CPP-NEXT: #include <utility>
// CPP-NEXT: #include <vector>
// CPP-EMPTY:
// CPP-NEXT: void bump(int& x, const long long& by) {
// CPP-NEXT:     x += static_cast<int>(by);
// CPP-NEXT:     x *= 2;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: long long walk(std::vector<int>& v, const std::vector<std::string>& names) {
// CPP-NEXT:     long long s = 0LL;
// CPP-NEXT:     for (int x : v) {
// CPP-NEXT:         s += static_cast<long long>(x);
// CPP-NEXT:     }
// CPP-NEXT:     for (const int x : v) {
// CPP-NEXT:         s += static_cast<long long>(x);
// CPP-NEXT:     }
// CPP-NEXT:     for (const int& x : v) {
// CPP-NEXT:         s += static_cast<long long>(x);
// CPP-NEXT:     }
// CPP-NEXT:     for (int& x : v) {
// CPP-NEXT:         ++x;
// CPP-NEXT:     }
// CPP-NEXT:     for (const std::string& w : names) {
// CPP-NEXT:         s += static_cast<long long>(w.size());
// CPP-NEXT:     }
// CPP-NEXT:     for (std::string w : names) {
// CPP-NEXT:         w += "!";
// CPP-NEXT:         s += static_cast<long long>(w.size());
// CPP-NEXT:     }
// CPP-NEXT:     return s;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: std::vector<int> keep(std::vector<int> v) {
// CPP-NEXT:     v.push_back(0);
// CPP-NEXT:     return v;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: std::vector<int> copy_of(const std::vector<int>& v) {
// CPP-NEXT:     return v;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int main() {
// CPP-NEXT:     int k = 3;
// CPP-NEXT:     bump(k, 10LL);
// CPP-NEXT:     std::vector<int> v = {1, 2};
// CPP-NEXT:     std::vector<std::string> names = {std::string("a"), std::string("bb")};
// CPP-NEXT:     long long w = walk(v, names);
// CPP-NEXT:     std::vector<int> c = v;
// CPP-NEXT:     std::vector<int> m = std::move(c);
// CPP-NEXT:     std::vector<int> kept = keep(v);
// CPP-NEXT:     std::vector<int> moved = keep(std::move(m));
// CPP-NEXT:     std::vector<std::vector<int>> all;
// CPP-NEXT:     all.push_back(kept);
// CPP-NEXT:     all.push_back(std::move(moved));
// CPP-NEXT:     std::string s = "x";
// CPP-NEXT:     names.push_back(s);
// CPP-NEXT:     names.push_back(std::move(s));
// CPP-NEXT:     std::printf("%d %lld %zu %zu %zu\n", k, w, all.size(), copy_of(v).size(), names.size());
// CPP-NEXT:     return 0;
// CPP-NEXT: }
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

void bump(int& x, const long long& by) {
    x += static_cast<int>(by);
    x = x * 2;
}

long long walk(std::vector<int>& v, const std::vector<std::string>& names) {
    long long s = 0;
    for (int x : v) {
        s += x;
    }
    for (const int x : v) {
        s += x;
    }
    for (const int& x : v) {
        s += x;
    }
    for (int& x : v) {
        x += 1;
    }
    for (const std::string& w : names) {
        s += static_cast<long long>(w.size());
    }
    for (std::string w : names) {
        w += "!";
        s += static_cast<long long>(w.size());
    }
    return s;
}

std::vector<int> keep(std::vector<int> v) {
    v.push_back(0);
    return v;
}

std::vector<int> copy_of(const std::vector<int>& v) {
    return v;
}

int main() {
    int k = 3;
    bump(k, 10);
    std::vector<int> v = {1, 2};
    std::vector<std::string> names = {"a", "bb"};
    long long w = walk(v, names);
    std::vector<int> c = v;
    std::vector<int> m = std::move(c);
    std::vector<int> kept = keep(v);
    std::vector<int> moved = keep(std::move(m));
    std::vector<std::vector<int>> all;
    all.push_back(kept);
    all.push_back(std::move(moved));
    std::string s = "x";
    names.push_back(s);
    names.push_back(std::move(s));
    std::printf("%d %lld %zu %zu %zu\n", k, w, all.size(), copy_of(v).size(), names.size());
    return 0;
}
