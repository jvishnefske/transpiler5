// FR-253: --emit=kernel shows the algorithm rows as the Kernel holds them:
// one `alg` node per row naming its operand vector (never an iterator), a
// callable argument as the bare closure variable (`alg sort_by v desc`), the
// back_inserter row as `extend_map w from v sq`, std::fill's int literal
// already the element type (`2:i64`), and the two C++ spellings without a
// Rust twin as negations: `std::none_of` is `(not (alg any_of ..))` (N25),
// `v.end() == std::find(..)` is `(not (alg contains ..))` (N26). std::count's
// literal is converted to the element type (`9:i64`: it fits, so the
// comparison is the same), *max_element is `(alg max_element v)`, std::max
// is `(max ..)`.
// RUN: %emitrust-bx --emit=kernel %s | FileCheck %s
// CHECK:      func f(v: &mut Vec<i32>, w: &mut Vec<i64>, mut x: i32) -> i64
// CHECK-NEXT:   block
// CHECK-NEXT:     closure move p(mut a: i32) -> bool [x]
// CHECK-NEXT:       block
// CHECK-NEXT:         return (lt a x)
// CHECK-NEXT:     closure sq(mut a: i32) -> i64 []
// CHECK-NEXT:       block
// CHECK-NEXT:         return (mul (cast i64 a) (cast i64 a))
// CHECK-NEXT:     closure desc(mut a: i32, mut b: i32) -> bool []
// CHECK-NEXT:       block
// CHECK-NEXT:         return (gt a b)
// CHECK-NEXT:     alg sort_by v desc
// CHECK-NEXT:     alg fill w 2:i64
// CHECK-NEXT:     alg extend_map w from v sq
// CHECK-NEXT:     let mut none: bool = (and (not (alg any_of v p)) (not (alg contains v x)))
// CHECK-NEXT:     let mut n: i64 = (add (alg count w 9:i64) (alg accumulate v 0:i64))
// CHECK-NEXT:     let mut hi: i32 = (max (alg max_element v) x)
// CHECK-NEXT:     return (add (add n (cast i64 hi)) (cond none 1:i64 0:i64))
#include <algorithm>
#include <iterator>
#include <numeric>
#include <vector>

long long f(std::vector<int>& v, std::vector<long long>& w, int x) {
    auto p = [x](int a) -> bool {
        return a < x;
    };
    auto sq = [](int a) -> long long {
        return static_cast<long long>(a) * static_cast<long long>(a);
    };
    auto desc = [](int a, int b) -> bool {
        return a > b;
    };
    std::sort(v.begin(), v.end(), desc);
    std::fill(w.begin(), w.end(), 2);
    std::transform(v.begin(), v.end(), std::back_inserter(w), sq);
    bool none = std::none_of(v.begin(), v.end(), p) && v.end() == std::find(v.begin(), v.end(), x);
    long long n = std::count(w.begin(), w.end(), 9) + std::accumulate(v.begin(), v.end(), 0LL);
    int hi = std::max(*std::max_element(v.begin(), v.end()), x);
    return n + hi + (none ? 1LL : 0LL);
}
