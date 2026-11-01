// REQUIRES: cargo
// FR-261: emitrust-bx C++ -> Rust, scalar and container OUT-PARAMETERS end
// to end (byte-diff vs clang++). `int*`, `bool*`, `double*`, `unsigned*` out
// parameters written through `*p` <=> `p[0usize]` and passed `&x` <=>
// `std::slice::from_mut(&mut x)`; a `std::vector<int>*` and a `std::string*`
// grown through `->push_back` / `+=` and read through `->size()` / `(*v)[i]`;
// the address of an ELEMENT `&v[i]` (normal form N17: the suffix `v.data() +
// i`, NOT a one-element slice -- `MaxOf(&xs[1], 3)` reads xs[1..4], defined
// C++ that a one-element `from_mut` would turn into a panic); the address of a
// vector element's data member `&ps[1].count`; an unsigned index `p[u]` and a
// promoted `unsigned char` index (N19: `static_cast<std::size_t>(..)` <=>
// `.. as usize`). UB-free; distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_ptr_outparams > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

struct Tally {
    int count;
    double mean;
};

bool DivMod(int a, int b, int* q, int* r) {
    if (b == 0) {
        return false;
    }
    *q = a / b;
    *r = a % b;
    return true;
}

void Stats(const int* xs, std::size_t n, double* mean, bool* any_neg) {
    long long s = 0;
    *any_neg = false;
    for (std::size_t i = 0; i < n; ++i) {
        s += xs[i];
        if (xs[i] < 0) {
            *any_neg = true;
        }
    }
    *mean = static_cast<double>(s) / static_cast<double>(n);
}

int MaxOf(const int* xs, std::size_t n) {
    int m = xs[0];
    for (std::size_t i = 1; i < n; ++i) {
        if (xs[i] > m) {
            m = xs[i];
        }
    }
    return m;
}

void Collect(std::vector<int>* out, std::string* log, int v) {
    out->push_back(v);
    *log += "+";
    log->push_back(static_cast<char>('a' + static_cast<int>(out->size())));
    (*out)[0] = (*out)[0] + v;
}

void Count(int* c, unsigned* hits, unsigned k) {
    *c += 1;
    hits[k] += 2U;
}

int Lookup(const int* table, unsigned char key) {
    return table[key];
}

int main() {
    int q = 0;
    int r = 0;
    const bool ok = DivMod(47, 5, &q, &r);
    const bool bad = DivMod(3, 0, &q, &r);
    std::printf("%d %d %d %d\n", static_cast<int>(ok), static_cast<int>(bad), q, r);
    std::vector<int> xs = {4, -9, 16, 25, 3};
    double mean = 0.0;
    bool neg = true;
    Stats(xs.data(), xs.size(), &mean, &neg);
    std::printf("%.3f %d\n", mean, static_cast<int>(neg));
    Stats(&xs[2], 3, &mean, &neg);
    std::printf("%.3f %d max=%d max1=%d\n", mean, static_cast<int>(neg), MaxOf(xs.data(), 5), MaxOf(&xs[1], 3));
    std::vector<int> got = {100};
    std::string log = "L";
    Collect(&got, &log, 7);
    Collect(&got, &log, 12);
    std::printf("%zu %d %d %s\n", got.size(), got[0], got[2], log.c_str());
    std::vector<Tally> ps = {{1, 0.5}, {2, 1.5}};
    std::vector<unsigned> hits(4, 1U);
    Count(&ps[1].count, hits.data(), 3U);
    Count(&ps[0].count, hits.data() + 1, 0U);
    std::printf("%d %d %u %u %u\n", ps[0].count, ps[1].count, hits[0], hits[1], hits[3]);
    std::vector<int> table = {10, 20, 30, 40};
    const unsigned char key = 2;
    std::printf("%d %d\n", Lookup(table.data(), key), Lookup(table.data() + 1, 2));
    return 0;
}
