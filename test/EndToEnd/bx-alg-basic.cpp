// REQUIRES: cargo
// FR-253: emitrust-bx C++ -> Rust, the <algorithm>/<numeric> rows that take
// no callable, over WHOLE vectors (byte-diff vs clang++). std::sort <=>
// sort_unstable on integers and strings (equal elements are
// indistinguishable, so the algorithm cannot be observed), stable_sort <=>
// sort, reverse, fill (an int literal / an int variable filling long longs:
// the conversion is an explicit cast), `std::find(..) != v.end()` <=>
// contains and `== v.end()` <=> !contains, std::count (ptrdiff_t: i64, not
// usize), std::accumulate in the INITIAL VALUE's type (`0LL` over ints sums in
// long long; unsigned wraps), *max_element / *min_element on integers and
// std::max / std::min. Inputs come from a small LCG seeded per call site, so
// no value is a compile-time constant. UB-free (checked by hand under
// -fsanitize=address,undefined).
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_alg_basic > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <algorithm>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

std::vector<int> make(int n, int seed, int range) {
    std::vector<int> v;
    unsigned x = static_cast<unsigned>(seed);
    for (int i = 0; i < n; ++i) {
        x = x * 1103515245U + 12345U;
        v.push_back(static_cast<int>((x >> 16) % static_cast<unsigned>(range)) - range / 2);
    }
    return v;
}

void show(const std::vector<int>& v) {
    for (int x : v) {
        std::printf(" %d", x);
    }
    std::printf("\n");
}

void sorts(int seed) {
    std::vector<int> v = make(23, seed, 40);
    std::sort(v.begin(), v.end());
    std::printf("sort:");
    show(v);
    std::vector<int> w = make(9, seed + 1, 7);
    std::stable_sort(w.begin(), w.end());
    std::reverse(w.begin(), w.end());
    std::printf("stable+reverse:");
    show(w);
    std::vector<unsigned> u = {4000000000U, 3U, static_cast<unsigned>(seed), 17U};
    std::sort(u.begin(), u.end());
    std::printf("unsigned: %u %u %u %u\n", u[0], u[1], u[2], u[3]);
    std::vector<std::string> s = {"pear", "apple", "fig", "apple pie", "Zed"};
    std::sort(s.begin(), s.end());
    std::reverse(s.begin(), s.end());
    for (const std::string& t : s) {
        std::printf("[%s]", t.c_str());
    }
    std::printf("\n");
    std::vector<char> c = {'q', 'a', 'z', 'a', 'm'};
    std::stable_sort(c.begin(), c.end());
    std::printf("chars: %c%c%c%c%c\n", c[0], c[1], c[2], c[3], c[4]);
}

void fills(int seed) {
    std::vector<long long> big(5, 1LL);
    std::fill(big.begin(), big.end(), 7);
    long long first = big[0] + big[4];
    std::fill(big.begin(), big.end(), seed);
    std::printf("fill: %lld %lld %lld\n", first, big[0], big[2]);
    std::vector<std::string> names(3, std::string("x"));
    std::string who = "bo";
    std::fill(names.begin(), names.end(), who);
    names[1] += "b";
    std::printf("fill strings: %s %s %s %s\n", names[0].c_str(), names[1].c_str(), names[2].c_str(), who.c_str());
}

void searches(int seed) {
    std::vector<int> v = make(31, seed, 20);
    int hits = 0;
    for (int k = -12; k < 12; ++k) {
        if (std::find(v.begin(), v.end(), k) != v.end()) {
            hits += 1;
        }
        long long c = std::count(v.begin(), v.end(), k);
        std::printf("%lld", c);
    }
    bool absent = std::find(v.begin(), v.end(), 99) == v.end();
    std::vector<long long> big = {5LL, -5LL, 5LL};
    long long fives = std::count(big.begin(), big.end(), 5);
    std::vector<std::string> s = {"a", "b", "a"};
    std::string a = "a";
    bool has = std::find(s.begin(), s.end(), a) != s.end();
    std::printf("\nfind: %d %d %lld %lld %d\n", hits, absent, fives, std::count(s.begin(), s.end(), a), has);
}

void sums(int seed) {
    std::vector<int> v = make(17, seed, 1000);
    int s = std::accumulate(v.begin(), v.end(), 0);
    long long wide = std::accumulate(v.begin(), v.end(), 2000000000LL);
    std::vector<unsigned> u = {4000000000U, 300000000U, static_cast<unsigned>(seed)};
    unsigned wrapped = std::accumulate(u.begin(), u.end(), 7U);
    std::vector<double> d = {0.5, -1.25, 1e10, 3.0};
    double dd = std::accumulate(d.begin(), d.end(), -0.0);
    std::vector<double> none;
    double zero = std::accumulate(none.begin(), none.end(), 0.0);
    double neg = std::accumulate(none.begin(), none.end(), -0.0);
    double mixed = std::accumulate(v.begin(), v.end(), 0.25);
    std::vector<unsigned char> bytes = {200, 100, 255};
    int bsum = std::accumulate(bytes.begin(), bytes.end(), 0);
    std::printf("sum: %d %lld %u %.3f %.1f %.1f %.2f %d\n", s, wide, wrapped, dd, zero, neg, mixed, bsum);
}

void extremes(int seed) {
    std::vector<int> v = make(13, seed, 100);
    int hi = *std::max_element(v.begin(), v.end());
    int lo = *std::min_element(v.begin(), v.end());
    std::vector<unsigned long long> u = {3ULL, 18000000000000000000ULL, 9ULL};
    unsigned long long top = *std::max_element(u.begin(), u.end());
    int m = std::max(hi, seed) - std::min(lo, -seed);
    long long big = std::max(static_cast<long long>(hi) * 1000000LL, 5LL);
    std::size_t sz = std::min(v.size(), static_cast<std::size_t>(4));
    std::printf("extremes: %d %d %llu %d %lld %zu\n", hi, lo, top, m, big, sz);
}

int main() {
    sorts(11);
    sorts(4242);
    fills(-3);
    searches(7);
    searches(1234);
    sums(99);
    sums(31337);
    extremes(5);
    extremes(808);
    return 0;
}
