// REQUIRES: cargo
// FR-253: emitrust-bx C++ -> Rust, `std::sort(b, e, cmp)` <=>
// `bx::sort_by_less(&mut v, &cmp)` (byte-diff vs clang++). Rust's
// sort_unstable_by is NOT std::sort: with a comparator whose equivalence
// classes hold distinguishable values the two order them differently (FR-249
// spike (e), measured), so the helper is a safe port of libstdc++-15's
// introsort and must reproduce it EXACTLY: every tie order, and the very
// sequence of comparator calls (a by-reference counter and a wrapping hash of
// the arguments, in call order, are printed). The input `killer()` is
// McIlroy's adversary ("A Killer Adversary for Quicksort", 1999) run against
// libstdc++'s std::sort at n = 128: it drives introsort past its depth limit
// (2 * log2 n) into the HEAPSORT branch inside a real sort -- measured with an
// instrumented copy of the helper (heapsort of [28, 128)) and by comparison
// count (2524 in both). Sorted with `a / 2 < b / 2`, pairs {2k, 2k+1} are
// equivalent, so the heap's tie order is visible. Also: |a| < |b| over values
// in -5..5 (FR-249 (e)'s diverging case), every size 0..40 (the insertion-sort
// threshold is 16), a descending comparator. Strict weak orders only:
// anything else is undefined behaviour in C++. UB-free (ASan/UBSan by hand).
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_alg_sort > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <algorithm>
#include <cstdio>
#include <vector>

std::vector<int> killer() {
    std::vector<int> v = {109, 1, 89, 3, 123, 5, 87, 7, 107, 9, 85, 11, 117, 13, 83, 15, 105, 17,
                          81, 19, 125, 21, 79, 23, 103, 25, 28, 27, 126, 124, 122, 120, 118, 116,
                          114, 112, 110, 108, 106, 104, 102, 100, 98, 96, 94, 92, 90, 88, 86, 84,
                          82, 80, 115, 101, 121, 99, 113, 97, 127, 95, 111, 93, 119, 91, 0, 2, 4,
                          6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 78, 75, 76, 73, 74, 71, 72, 69,
                          70, 67, 68, 65, 66, 63, 64, 61, 62, 59, 60, 57, 58, 55, 56, 53, 54, 51,
                          52, 49, 50, 47, 48, 45, 46, 43, 44, 41, 42, 39, 40, 37, 38, 35, 36, 33,
                          34, 31, 32, 29, 30, 77};
    return v;
}

std::vector<int> make(int n, int seed, int range) {
    std::vector<int> v;
    unsigned x = static_cast<unsigned>(seed);
    for (int i = 0; i < n; ++i) {
        x = x * 1103515245U + 12345U;
        v.push_back(static_cast<int>((x >> 16) % static_cast<unsigned>(range)) - range / 2);
    }
    return v;
}

unsigned long long digest(const std::vector<int>& v) {
    unsigned long long h = 1469598103934665603ULL;
    for (int x : v) {
        h = (h ^ static_cast<unsigned long long>(x)) * 1099511628211ULL;
    }
    return h;
}

void killers() {
    std::vector<int> v = killer();
    long long calls = 0;
    unsigned long long trace = 0ULL;
    auto halves = [&calls, &trace](int a, int b) -> bool {
        calls += 1LL;
        trace = trace * 1000003ULL + static_cast<unsigned long long>(a * 131 + b);
        return a / 2 < b / 2;
    };
    std::sort(v.begin(), v.end(), halves);
    std::printf("killer/2: %lld %llu\n", calls, trace);
    for (int i = 0; i < 128; ++i) {
        std::printf(" %d", v[i]);
        if (i % 32 == 31) {
            std::printf("\n");
        }
    }
    std::vector<int> w = killer();
    long long plain = 0;
    auto lt = [&plain](int a, int b) -> bool {
        plain += 1LL;
        return a < b;
    };
    std::sort(w.begin(), w.end(), lt);
    std::printf("killer: %lld %d %d %llu\n", plain, w[0], w[127], digest(w));
}

void abs_order(int seed) {
    std::vector<int> v = make(300, seed, 11);
    auto by_abs = [](int a, int b) -> bool {
        return (a < 0 ? -a : a) < (b < 0 ? -b : b);
    };
    std::sort(v.begin(), v.end(), by_abs);
    std::printf("abs %d: %llu", seed, digest(v));
    for (int i = 0; i < 25; ++i) {
        std::printf(" %d", v[i * 12]);
    }
    std::printf("\n");
}

void sizes() {
    unsigned long long all = 0ULL;
    long long total = 0;
    for (int n = 0; n < 41; ++n) {
        std::vector<int> v = make(n, n * 7 + 1, 50);
        auto last_digit = [&total](int a, int b) -> bool {
            total += 1LL;
            return (a + 100) % 10 < (b + 100) % 10;
        };
        std::sort(v.begin(), v.end(), last_digit);
        all = all * 31ULL + digest(v);
    }
    std::vector<int> d = make(57, 3, 1000);
    auto desc = [](int a, int b) -> bool {
        return a > b;
    };
    std::sort(d.begin(), d.end(), desc);
    std::printf("sizes: %llu %lld %d %d %d\n", all, total, d[0], d[28], d[56]);
}

int main() {
    killers();
    abs_order(1);
    abs_order(99);
    sizes();
    return 0;
}
