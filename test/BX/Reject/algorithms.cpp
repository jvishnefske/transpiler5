// FR-253: the nearest out-of-subset neighbour of every algorithm row is a
// LOCATED refusal, each naming why (and the rewrite, where one exists):
// free iterators (`v.begin() + 1`, begin/end of two containers, an iterator
// difference, std::find compared with another container's end, a binary
// std::transform); a std::string range; the f64 rows (sort without a
// comparator, *max_element, std::max -- -0.0 / 0.0 and NaN are
// distinguishable); stable_sort with a comparator; std::count of a value of
// another type and std::accumulate whose initial value cannot hold the
// elements (C++ sums in the INIT's type: `0` over long longs truncates every
// step); std::fill reading the vector it fills and std::transform appending
// to its own source (a second borrow in Rust); callable arguments that are
// not a local closure of exactly the called signature -- an inline lambda
// (normalizer: bind it first), a function, a callable parameter, a `mutable`
// lambda (C++ copies it into the algorithm), a widening result, a non-void
// for_each body; and the FR-252 / FR-263 analyses seeing the algorithm call
// as a call of its closure: a closure that grows the vector being iterated, a
// comparator reading the vector being sorted, two printing count_ifs in one
// statement, and a closure shrinking a counting loop's bound.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}25{{$}}
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <numeric>
#include <string>
#include <vector>

bool pos(int a) {
    return a > 0;
}

bool lt(int a, int b) {
    return a < b;
}

long long rows(std::vector<int>& v, std::vector<int>& u, std::vector<long long>& big, std::vector<double>& d,
               std::string& s) {
// CHECK:      algorithms.cpp:[[@LINE+1]]:5: error: unsupported: std::sort over something other than a whole container `v.begin(), v.end()` (free iterators are outside the subset)
    std::sort(v.begin() + 1, v.end());
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:5: error: unsupported: std::reverse over the begin() and end() of two different containers
    std::reverse(v.begin(), u.end());
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:5: error: unsupported: std::sort over a std::string (the subset's algorithms take a std::vector)
    std::sort(s.begin(), s.end());
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:5: error: unsupported: std::sort of a vector of double without a comparator (-0.0 and 0.0 are equivalent but print differently, so the result depends on the algorithm; f64 has no total order in Rust)
    std::sort(d.begin(), d.end());
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:5: error: unsupported: std::stable_sort with a comparator
    std::stable_sort(v.begin(), v.end(), lt);
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:54: error: unsupported: std::count of a value of type i32 in a vector of i64 (C++ would compare after the usual arithmetic conversions; normalizer: static_cast the value to the element type)
    long long c = std::count(big.begin(), big.end(), v[0]);
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:53: error: unsupported: std::accumulate of i64 elements from an initial value of type i32 (C++ sums in the initial value's type, converting every step; normalizer: give the initial value a type that holds every element, e.g. `0LL` / `0i64`)
    int t = std::accumulate(big.begin(), big.end(), 0);
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:18: error: unsupported: std::max_element over a vector of double (it returns the FIRST extreme, and -0.0 / 0.0 compare equal but print differently; f64 has no total order in Rust)
    double hi = *std::max_element(d.begin(), d.end());
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:17: error: unsupported: std::max of two doubles (it returns its first argument unless the second compares greater, which differs from f64::max on NaN and signed zero)
    double mx = std::max(d[0], d[1]);
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:35: error: unsupported: the value of std::fill reads 'v', the vector it fills (a second borrow in Rust)
    std::fill(v.begin(), v.end(), v[0]);
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:54: error: unsupported: std::vector::begin (iterators and pointers are outside the subset)
    int top = std::max_element(v.begin(), v.end()) - v.begin();
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:15: error: unsupported: std::find compared with something other than the same container's end() (iterators are outside the subset)
    bool at = std::find(v.begin(), v.end(), 3) != u.end();
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:5: error: unsupported: std::transform over two ranges (a binary operation)
    std::transform(v.begin(), v.end(), v.begin(), v.begin(), std::plus<int>());
    return c + t + static_cast<long long>(hi + mx) + (at ? 1 : 0) + top;
}

long long callables(std::vector<int>& v, std::vector<int>& w) {
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:53: error: unsupported: lambda expression as an argument of std::count_if (normalizer: bind it to a local closure first, `auto f = [..](..) {..};`, and pass `f`)
    long long a = std::count_if(v.begin(), v.end(), [](int x) -> bool { return x > 1; });
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:53: error: unsupported: function 'pos' passed to std::count_if (the subset passes a local closure)
    long long b = std::count_if(v.begin(), v.end(), pos);
    int k = 0;
    auto step = [k](int x) mutable -> bool {
        k += x;
        return k > 3;
    };
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:53: error: unsupported: closure 'step' modifies its own copy of a capture (a `mutable` lambda) and is passed to std::count_if, which copies it in C++ while Rust would borrow it (normalizer: capture the state by reference)
    long long c = std::count_if(v.begin(), v.end(), step);
    auto wide = [](int x) -> long long {
        return x;
    };
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:51: error: unsupported: closure 'wide' has type impl Fn(i32) -> i64, but std::transform calls it as impl Fn(i32) -> i32 (the subset has no implicit conversion of an argument or result)
    std::transform(v.begin(), v.end(), v.begin(), wide);
    auto keep = [](int x) -> int {
        return x;
    };
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:5: error: unsupported: std::transform appends to 'v' while reading it (iterator invalidation in C++, a second borrow in Rust)
    std::transform(v.begin(), v.end(), std::back_inserter(v), keep);
    auto each = [](int x) -> int {
        return x;
    };
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:39: error: unsupported: closure 'each' has type impl Fn(i32) -> i32, but std::for_each calls it as impl Fn(i32) (the subset has no implicit conversion of an argument or result)
    std::for_each(w.begin(), w.end(), each);
    return a + b + c;
}

long long effects(std::vector<int>& v, int n) {
    auto grow = [&v](int a) {
        v.push_back(a);
    };
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:19: error: unsupported: 'v' is used while closure 'grow', which modifies it through a by-reference capture, is still called later (a second borrow in Rust)
    std::for_each(v.begin(), v.end(), grow);
    std::vector<int> w = {1, 2};
    auto peek = [&w](int a, int b) -> bool {
        return a + w[0] < b;
    };
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:5: error: unsupported: 'w' is modified or moved while closure 'peek', which captures it by reference, is still called later (a borrow conflict in Rust)
    std::sort(w.begin(), w.end(), peek);
    auto loud = [](int a) -> bool {
        std::printf("%d\n", a);
        return a > 0;
    };
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:64: error: unsupported: two calls that may print in one statement without a sequence point between them (C++ leaves their order unspecified; normalizer: evaluate each into a local first)
    long long both = std::count_if(v.begin(), v.end(), loud) + std::count_if(v.begin(), v.end(), loud);
    auto shrink = [&n](int a) -> bool {
        n -= 1;
        return a > 0;
    };
    long long s = 0;
// CHECK-NEXT: algorithms.cpp:[[@LINE+2]]:5: error: unsupported: counting loop body assigns or shadows the bound 'n'
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:25: error: unsupported: 'n' is used while closure 'shrink', which modifies it through a by-reference capture, is still called later (a second borrow in Rust)
    for (int i = 0; i < n; ++i) {
        s += std::count_if(v.begin(), v.end(), shrink);
    }
    return both + s;
}

template <typename F>
long long via(const std::vector<int>& v, const F& f) {
// CHECK-NEXT: algorithms.cpp:[[@LINE+1]]:46: error: unsupported: callable parameter 'f' passed to std::count_if (the subset passes a local closure to an algorithm)
    return std::count_if(v.begin(), v.end(), f);
}

int main() {
    std::vector<int> v = {1, 2};
    auto p = [](int a) -> bool {
        return a > 1;
    };
    std::printf("%lld\n", via(v, p));
    return 0;
}
