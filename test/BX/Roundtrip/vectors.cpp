// FR-251: the std::vector rows, C++ -> canonical Rust under --check-roundtrip
// (laws 1-2 asserted on BOTH sides for this input), plus the canonical C++
// the Kernel prints back (CPP). Pins every vector row: `std::vector<T> v;` /
// `= {..}` / `(n, x)` / nested braces <=> Vec::new / vec![..] / vec![x; n];
// push_back/pop_back/resize/clear <=> push/pop/resize/clear; size/empty <=>
// len/is_empty; `v[i]` with an int index (an explicit `as usize`), compound
// `+=` and `++v[i]` <=> `v[i] += 1`; front()/back() desugar to `v[0]` /
// `v[v.size() - 1]` (the C++ re-print shows the desugaring); ==/!=;
// `const std::vector<T>&` <=> `&Vec<T>` (not `&[T]`: type constructors stay
// one-to-one), `std::vector<T>&` <=> `&mut Vec<T>` with `f(&g, &mut e)` at
// the call site; a counting loop bounded by `g.size()`; and the empty
// temporary, which is `Vec::<i32>::new()` in Rust (rustc cannot infer T
// there) and `std::vector<int>{}` in C++ (`std::vector<int>()` is a
// declarator ambiguity in some positions).
// RUN: %emitrust-bx --check-roundtrip %s | FileCheck %s
// RUN: %emitrust-bx --emit=cpp %s | FileCheck --check-prefix=CPP %s
// CHECK:      fn make(mut n: i32, mut fill: i32) -> Vec<i32> {
// CHECK-NEXT:     let mut v: Vec<i32> = vec![fill; n as usize];
// CHECK-NEXT:     return v;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn rows(g: &Vec<Vec<i32>>, out: &mut Vec<i32>) -> i32 {
// CHECK-NEXT:     out.clear();
// CHECK-NEXT:     for r in 0usize..g.len() {
// CHECK-NEXT:         out.push(g[r].len() as i32);
// CHECK-NEXT:     }
// CHECK-NEXT:     return out.len() as i32;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn main() {
// CHECK-NEXT:     let mut e: Vec<i32> = Vec::new();
// CHECK-NEXT:     let mut v: Vec<i32> = vec![3, 1, 4];
// CHECK-NEXT:     let mut f: Vec<f64> = vec![0.5; 2usize];
// CHECK-NEXT:     let mut g: Vec<Vec<i32>> = vec![vec![1], vec![2, 3]];
// CHECK-NEXT:     v.push(1);
// CHECK-NEXT:     v.pop();
// CHECK-NEXT:     v.resize(5usize, 9);
// CHECK-NEXT:     let mut i: i32 = 1;
// CHECK-NEXT:     v[i as usize] = v[0usize] + v[0usize] + v[v.len().wrapping_sub(1usize)];
// CHECK-NEXT:     v[2usize] += 7;
// CHECK-NEXT:     v[3usize] += 1;
// CHECK-NEXT:     g[1usize].push(4);
// CHECK-NEXT:     g.push(vec![5, 6]);
// CHECK-NEXT:     g.push(Vec::<i32>::new());
// CHECK-NEXT:     let mut same: bool = v == make(5, 9);
// CHECK-NEXT:     print!("{} {} {} {} {}\n", v.len(), v.is_empty() as i32, e.is_empty() as i32, same as i32, (v != e) as i32);
// CHECK-NEXT:     print!("{} {}\n", rows(&g, &mut e), bx::fmt_f(f[1usize], 1));
// CHECK-NEXT: }
// CPP:      #include <cstdio>
// CPP-NEXT: #include <vector>
// CPP-EMPTY:
// CPP-NEXT: std::vector<int> make(int n, int fill) {
// CPP-NEXT:     std::vector<int> v(static_cast<std::size_t>(n), fill);
// CPP-NEXT:     return v;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int rows(const std::vector<std::vector<int>>& g, std::vector<int>& out) {
// CPP-NEXT:     out.clear();
// CPP-NEXT:     for (std::size_t r = static_cast<std::size_t>(0); r < g.size(); ++r) {
// CPP-NEXT:         out.push_back(static_cast<int>(g[r].size()));
// CPP-NEXT:     }
// CPP-NEXT:     return static_cast<int>(out.size());
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int main() {
// CPP-NEXT:     std::vector<int> e;
// CPP-NEXT:     std::vector<int> v = {3, 1, 4};
// CPP-NEXT:     std::vector<double> f(static_cast<std::size_t>(2), 0.5);
// CPP-NEXT:     std::vector<std::vector<int>> g = {std::vector<int>{1}, std::vector<int>{2, 3}};
// CPP-NEXT:     v.push_back(1);
// CPP-NEXT:     v.pop_back();
// CPP-NEXT:     v.resize(static_cast<std::size_t>(5), 9);
// CPP-NEXT:     int i = 1;
// CPP-NEXT:     v[static_cast<std::size_t>(i)] = v[static_cast<std::size_t>(0)] + v[static_cast<std::size_t>(0)] + v[v.size() - static_cast<std::size_t>(1)];
// CPP-NEXT:     v[static_cast<std::size_t>(2)] += 7;
// CPP-NEXT:     ++v[static_cast<std::size_t>(3)];
// CPP-NEXT:     g[static_cast<std::size_t>(1)].push_back(4);
// CPP-NEXT:     g.push_back(std::vector<int>{5, 6});
// CPP-NEXT:     g.push_back(std::vector<int>{});
// CPP-NEXT:     bool same = v == make(5, 9);
// CPP-NEXT:     std::printf("%zu %d %d %d %d\n", v.size(), static_cast<int>(v.empty()), static_cast<int>(e.empty()), static_cast<int>(same), static_cast<int>(v != e));
// CPP-NEXT:     std::printf("%d %.1f\n", rows(g, e), f[static_cast<std::size_t>(1)]);
// CPP-NEXT:     return 0;
// CPP-NEXT: }
#include <cstdio>
#include <vector>

std::vector<int> make(int n, int fill) {
    std::vector<int> v(n, fill);
    return v;
}

int rows(const std::vector<std::vector<int>>& g, std::vector<int>& out) {
    out.clear();
    for (std::size_t r = 0; r < g.size(); ++r) {
        out.push_back(static_cast<int>(g[r].size()));
    }
    return static_cast<int>(out.size());
}

int main() {
    std::vector<int> e;
    std::vector<int> v = {3, 1, 4};
    std::vector<double> f(2, 0.5);
    std::vector<std::vector<int>> g = {{1}, {2, 3}};
    v.push_back(1);
    v.pop_back();
    v.resize(5, 9);
    int i = 1;
    v[i] = v[0] + v.front() + v.back();
    v[2] += 7;
    ++v[3];
    g[1].push_back(4);
    g.push_back(std::vector<int>{5, 6});
    g.push_back(std::vector<int>());
    bool same = v == make(5, 9);
    std::printf("%zu %d %d %d %d\n", v.size(), v.empty(), e.empty(), same, v != e);
    std::printf("%d %.1f\n", rows(g, e), f[1]);
    return 0;
}
