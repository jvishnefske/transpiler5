// FR-253: <algorithm>/<numeric> over whole vectors, C++ -> canonical Rust
// under --check-roundtrip (laws 1-2 on BOTH sides), plus the canonical C++.
// Pins every row's two spellings: sort <=> sort_unstable, stable_sort <=>
// sort, reverse, fill (an int filling long longs is an explicit cast),
// `find(..) != end` <=> contains, and BOTH negated spellings (`v.end() ==
// std::find(..)` and `!(std::find(..) != v.end())`) print as the one
// canonical `std::find(..) == v.end()` (N26); a char literal searched in ints
// is the int it promotes to; count is i64 (an int destination is an explicit
// cast); accumulate in the init's type (`0LL` over ints: `e as i64`);
// *max_element / *min_element; std::max / std::min <=> std::cmp::max / min;
// count_if / any_of / all_of, none_of <=> `!..any` (N25), accumulate with an
// operation, transform in place and into std::back_inserter of a data member,
// the comparator sort <=> bx::sort_by_less, for_each. A closure that modifies
// state is `let mut` and passed `&mut`; containers reached through `T&`
// parameters print bare in the helpers (`bx::count_if(v, ..)`), owned places
// borrowed (`&b.xs`, `&mut b.xs`).
// RUN: %emitrust-bx --check-roundtrip %s | FileCheck %s
// RUN: %emitrust-bx --emit=cpp %s | FileCheck --check-prefix=CPP %s
// CHECK:      fn rows(v: &mut Vec<i32>, names: &Vec<String>, b: &mut Bin, mut x: i32) -> i64 {
// CHECK-NEXT:     v.sort_unstable();
// CHECK-NEXT:     b.xs.sort();
// CHECK-NEXT:     b.out.reverse();
// CHECK-NEXT:     b.out.fill(x as i64);
// CHECK-NEXT:     let mut has: bool = v.contains(&x);
// CHECK-NEXT:     let mut gone: bool = !v.contains(&97);
// CHECK-NEXT:     let mut gone2: bool = !names.contains(&names[0usize]);
// CHECK-NEXT:     let mut c: i32 = bx::count(&b.xs, &4) as i32;
// CHECK-NEXT:     let mut s: i64 = v.iter().fold(0i64, |a, &e| a + e as i64);
// CHECK-NEXT:     let mut u: u32 = (v.iter().fold(0, |a, &e| a + e) as u32).wrapping_add(1u32);
// CHECK-NEXT:     let mut m: i32 = std::cmp::max(*v.iter().max().unwrap(), x) - std::cmp::min(x, *b.xs.iter().min().unwrap());
// CHECK-NEXT:     print!("{} {} {} {} {} {}\n", has as i32, gone as i32, gone2 as i32, c, u, m);
// CHECK-NEXT:     return s;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn closures(v: &mut Vec<i32>, b: &mut Bin) -> i32 {
// CHECK-NEXT:     let mut calls: i32 = 0;
// CHECK-NEXT:     let mut big = |mut a: i32| -> bool {
// CHECK-NEXT:         calls += 1;
// CHECK-NEXT:         return a > 9;
// CHECK-NEXT:     };
// CHECK-NEXT:     let plus = |mut acc: i32, mut a: i32| -> i32 {
// CHECK-NEXT:         return acc + a;
// CHECK-NEXT:     };
// CHECK-NEXT:     let wide = |mut a: i32| -> i64 {
// CHECK-NEXT:         return (a as i64) * 3i64;
// CHECK-NEXT:     };
// CHECK-NEXT:     let neg = |mut a: i32| -> i32 {
// CHECK-NEXT:         return -a;
// CHECK-NEXT:     };
// CHECK-NEXT:     let desc = |mut a: i32, mut b2: i32| -> bool {
// CHECK-NEXT:         return a > b2;
// CHECK-NEXT:     };
// CHECK-NEXT:     let show = |mut a: i32| {
// CHECK-NEXT:         print!("{}\n", a);
// CHECK-NEXT:     };
// CHECK-NEXT:     let mut n: i64 = bx::count_if(v, &mut big);
// CHECK-NEXT:     let mut any: bool = b.xs.iter().copied().any(&mut big);
// CHECK-NEXT:     let mut none: bool = !v.iter().copied().any(&mut big);
// CHECK-NEXT:     let mut all: bool = v.iter().copied().all(&mut big);
// CHECK-NEXT:     let mut t: i32 = v.iter().copied().fold(10, &plus);
// CHECK-NEXT:     bx::transform(v, &neg);
// CHECK-NEXT:     b.out.extend(v.iter().copied().map(&wide));
// CHECK-NEXT:     bx::sort_by_less(&mut b.xs, &desc);
// CHECK-NEXT:     v.iter().copied().for_each(&show);
// CHECK-NEXT:     return (n as i32) + t + calls + (if any { 1 } else { 0 }) + (if none { 2 } else { 0 }) + (if all { 4 } else { 0 });
// CHECK-NEXT: }
// CPP:      long long rows(std::vector<int>& v, const std::vector<std::string>& names, Bin& b, int x) {
// CPP-NEXT:     std::sort(v.begin(), v.end());
// CPP-NEXT:     std::stable_sort(b.xs.begin(), b.xs.end());
// CPP-NEXT:     std::reverse(b.out.begin(), b.out.end());
// CPP-NEXT:     std::fill(b.out.begin(), b.out.end(), static_cast<long long>(x));
// CPP-NEXT:     bool has = std::find(v.begin(), v.end(), x) != v.end();
// CPP-NEXT:     bool gone = std::find(v.begin(), v.end(), 97) == v.end();
// CPP-NEXT:     bool gone2 = std::find(names.begin(), names.end(), names[static_cast<std::size_t>(0)]) == names.end();
// CPP-NEXT:     int c = static_cast<int>(std::count(b.xs.begin(), b.xs.end(), 4));
// CPP-NEXT:     long long s = std::accumulate(v.begin(), v.end(), 0LL);
// CPP-NEXT:     unsigned u = static_cast<unsigned>(std::accumulate(v.begin(), v.end(), 0)) + 1U;
// CPP-NEXT:     int m = std::max(*std::max_element(v.begin(), v.end()), x) - std::min(x, *std::min_element(b.xs.begin(), b.xs.end()));
// CPP-NEXT:     std::printf("%d %d %d %d %u %d\n", static_cast<int>(has), static_cast<int>(gone), static_cast<int>(gone2), c, u, m);
// CPP-NEXT:     return s;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int closures(std::vector<int>& v, Bin& b) {
// CPP-NEXT:     int calls = 0;
// CPP-NEXT:     auto big = [&calls](int a) -> bool {
// CPP-NEXT:         ++calls;
// CPP-NEXT:         return a > 9;
// CPP-NEXT:     };
// CPP-NEXT:     auto plus = [](int acc, int a) -> int {
// CPP-NEXT:         return acc + a;
// CPP-NEXT:     };
// CPP-NEXT:     auto wide = [](int a) -> long long {
// CPP-NEXT:         return static_cast<long long>(a) * 3LL;
// CPP-NEXT:     };
// CPP-NEXT:     auto neg = [](int a) -> int {
// CPP-NEXT:         return -a;
// CPP-NEXT:     };
// CPP-NEXT:     auto desc = [](int a, int b2) -> bool {
// CPP-NEXT:         return a > b2;
// CPP-NEXT:     };
// CPP-NEXT:     auto show = [](int a) {
// CPP-NEXT:         std::printf("%d\n", a);
// CPP-NEXT:     };
// CPP-NEXT:     long long n = std::count_if(v.begin(), v.end(), big);
// CPP-NEXT:     bool any = std::any_of(b.xs.begin(), b.xs.end(), big);
// CPP-NEXT:     bool none = std::none_of(v.begin(), v.end(), big);
// CPP-NEXT:     bool all = std::all_of(v.begin(), v.end(), big);
// CPP-NEXT:     int t = std::accumulate(v.begin(), v.end(), 10, plus);
// CPP-NEXT:     std::transform(v.begin(), v.end(), v.begin(), neg);
// CPP-NEXT:     std::transform(v.begin(), v.end(), std::back_inserter(b.out), wide);
// CPP-NEXT:     std::sort(b.xs.begin(), b.xs.end(), desc);
// CPP-NEXT:     std::for_each(v.begin(), v.end(), show);
// CPP-NEXT:     return static_cast<int>(n) + t + calls + (any ? 1 : 0) + (none ? 2 : 0) + (all ? 4 : 0);
// CPP-NEXT: }
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <numeric>
#include <string>
#include <vector>

struct Bin {
    std::vector<int> xs;
    std::vector<long long> out;
};

long long rows(std::vector<int>& v, const std::vector<std::string>& names, Bin& b, int x) {
    std::sort(v.begin(), v.end());
    std::stable_sort(b.xs.begin(), b.xs.end());
    std::reverse(b.out.begin(), b.out.end());
    std::fill(b.out.begin(), b.out.end(), x);
    bool has = std::find(v.begin(), v.end(), x) != v.end();
    bool gone = v.end() == std::find(v.begin(), v.end(), 'a');
    bool gone2 = !(std::find(names.begin(), names.end(), names[0]) != names.end());
    int c = std::count(b.xs.begin(), b.xs.end(), 4);
    long long s = std::accumulate(v.begin(), v.end(), 0LL);
    unsigned u = std::accumulate(v.begin(), v.end(), 0) + 1U;
    int m = std::max(*std::max_element(v.begin(), v.end()), x) - std::min(x, *std::min_element(b.xs.begin(), b.xs.end()));
    std::printf("%d %d %d %d %u %d\n", has, gone, gone2, c, u, m);
    return s;
}

int closures(std::vector<int>& v, Bin& b) {
    int calls = 0;
    auto big = [&calls](int a) -> bool {
        calls += 1;
        return a > 9;
    };
    auto plus = [](int acc, int a) -> int {
        return acc + a;
    };
    auto wide = [](int a) -> long long {
        return static_cast<long long>(a) * 3LL;
    };
    auto neg = [](int a) -> int {
        return -a;
    };
    auto desc = [](int a, int b2) -> bool {
        return a > b2;
    };
    auto show = [](int a) {
        std::printf("%d\n", a);
    };
    long long n = std::count_if(v.begin(), v.end(), big);
    bool any = std::any_of(b.xs.begin(), b.xs.end(), big);
    bool none = std::none_of(v.begin(), v.end(), big);
    bool all = std::all_of(v.begin(), v.end(), big);
    int t = std::accumulate(v.begin(), v.end(), 10, plus);
    std::transform(v.begin(), v.end(), v.begin(), neg);
    std::transform(v.begin(), v.end(), std::back_inserter(b.out), wide);
    std::sort(b.xs.begin(), b.xs.end(), desc);
    std::for_each(v.begin(), v.end(), show);
    return static_cast<int>(n) + t + calls + (any ? 1 : 0) + (none ? 2 : 0) + (all ? 4 : 0);
}

int main() {
    std::vector<int> v = {3, 12, 5};
    std::vector<std::string> names = {"x", "y"};
    Bin b = {{4, 1, 4}, {7LL}};
    long long r = rows(v, names, b, 5);
    int k = closures(v, b);
    std::printf("%lld %d %zu\n", r, k, b.out.size());
    return 0;
}
