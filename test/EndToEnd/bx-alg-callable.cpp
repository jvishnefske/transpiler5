// REQUIRES: cargo
// FR-253: emitrust-bx C++ -> Rust, the algorithm rows that call a LOCAL
// closure (byte-diff vs clang++): count_if <=> bx::count_if, any_of /
// all_of / none_of <=> `.iter().copied().any/all(&p)` (none_of is !any_of),
// accumulate with an operation <=> `.copied().fold(init, &f)`, for_each <=>
// `.copied().for_each(&f)`, transform in place <=> bx::transform, and
// transform into std::back_inserter(w) <=> `w.extend(v.iter().copied().
// map(&f))`. The load-bearing rows: a closure that MODIFIES a by-reference
// capture (a call counter, a running total) -- the algorithm call IS a call of
// the closure, so the effect lands in both languages, and the closure is
// passed as `&mut f`; a by-copy closure whose captured value changes after it
// is made (the algorithm sees the copy); an accumulate whose operation is not
// commutative (the left-to-right order is observable); a predicate that
// prints (one algorithm per statement: FR-263). UB-free, inputs from an LCG.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_alg_callable > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <numeric>
#include <vector>

std::vector<int> make(int n, int seed) {
    std::vector<int> v;
    unsigned x = static_cast<unsigned>(seed);
    for (int i = 0; i < n; ++i) {
        x = x * 1103515245U + 12345U;
        v.push_back(static_cast<int>((x >> 16) % 61U) - 30);
    }
    return v;
}

void predicates(int seed) {
    std::vector<int> v = make(25, seed);
    int lim = seed % 7;
    auto big = [lim](int a) -> bool {
        return a > lim;
    };
    lim = 1000;
    int seen = 0;
    auto neg = [&seen](int a) -> bool {
        seen += 1;
        return a < 0;
    };
    long long n = std::count_if(v.begin(), v.end(), big);
    long long m = std::count_if(v.begin(), v.end(), neg);
    bool any = std::any_of(v.begin(), v.end(), neg);
    bool all = std::all_of(v.begin(), v.end(), neg);
    bool none = std::none_of(v.begin(), v.end(), big);
    std::printf("pred: %lld %lld %d %d %d %d %d\n", n, m, any, all, seen, none, lim);
}

void folds(int seed) {
    std::vector<int> v = make(12, seed);
    auto horner = [](long long acc, int d) -> long long {
        return acc * 3LL + static_cast<long long>(d);
    };
    long long h = std::accumulate(v.begin(), v.end(), 1LL, horner);
    int steps = 0;
    auto maxer = [&steps](int acc, int a) -> int {
        steps += 1;
        if (a > acc) {
            return a;
        }
        return acc;
    };
    int best = std::accumulate(v.begin(), v.end(), -100, maxer);
    std::vector<double> d = {0.5, 0.25, 4.0};
    auto halve = [](double acc, double x) -> double {
        return acc / 2.0 + x;
    };
    double hd = std::accumulate(d.begin(), d.end(), static_cast<double>(seed), halve);
    std::printf("fold: %lld %d %d %.4f\n", h, best, steps, hd);
}

void effects(int seed) {
    std::vector<int> v = make(10, seed);
    long long total = 0;
    int calls = 0;
    auto add = [&total, &calls](int a) {
        total = total * 7LL + static_cast<long long>(a);
        calls += 1;
    };
    std::for_each(v.begin(), v.end(), add);
    std::for_each(v.begin(), v.end(), add);
    auto shout = [](int a) {
        std::printf("<%d>", a);
    };
    std::for_each(v.begin(), v.end(), shout);
    std::printf("\neach: %lld %d\n", total, calls);
}

void transforms(int seed) {
    std::vector<int> v = make(8, seed);
    int k = seed % 5;
    auto twice = [k](int a) -> int {
        return a * 2 + k;
    };
    std::transform(v.begin(), v.end(), v.begin(), twice);
    std::vector<long long> sq = {99LL};
    auto widen = [](int a) -> long long {
        return static_cast<long long>(a) * static_cast<long long>(a) * 100000LL;
    };
    std::transform(v.begin(), v.end(), std::back_inserter(sq), widen);
    int order = 0;
    auto stamp = [&order](int a) -> int {
        order += 1;
        return a * 100 + order;
    };
    std::transform(v.begin(), v.end(), v.begin(), stamp);
    std::printf("transform:");
    for (int x : v) {
        std::printf(" %d", x);
    }
    std::printf(" |");
    for (long long x : sq) {
        std::printf(" %lld", x);
    }
    std::printf(" | %d %zu\n", order, sq.size());
}

int main() {
    predicates(3);
    predicates(77);
    folds(5);
    folds(2024);
    effects(9);
    transforms(13);
    transforms(-8);
    return 0;
}
