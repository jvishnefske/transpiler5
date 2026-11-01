// REQUIRES: cargo
// FR-257: emitrust-bx C++ -> Rust, constructors end to end (byte-diff vs
// clang++). A constructor whose body is a member-initializer list of every
// data member <=> `pub fn new(..) -> S { return S { .. }; }`; every C++
// spelling of a call -- `S s(a, b);`, `S s{a, b};`, `S s = S(a, b);`, a
// temporary `S(a, b)` argument, `S{..}` through the constructor -- <=>
// `S::new(a, b)`. The initializers run in DECLARATION order in C++ whatever
// order they are written in (`Pair`'s are written backwards; clang lists
// them in declaration order) and a Rust struct literal runs in written
// order, so the canonical spelling writes them in declaration order. A
// by-value std::string parameter is copied into its member (`s(s0)` <=>
// `s: s0.clone()`) or moved (`std::move(c)` <=> `c`); a member initializer
// may call a function and a member function of another struct. UB-free;
// distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_ctors > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

int next(std::vector<int>& log, int v) {
    log.push_back(v);
    return v * 10 + static_cast<int>(log.size());
}

struct Pair {
    int first;
    int second;
    Pair(int a, int b) : second(b - 1), first(a + 1) {}
};

struct Named {
    std::string name;
    std::string copy;
    long long weight;
    Named(std::string n, std::string c, long long w) : name(n), copy(std::move(c)), weight(w * 3LL) {}
    long long score() const {
        return weight + static_cast<long long>(name.size()) * 100LL;
    }
};

struct Logged {
    int a;
    int b;
    Logged(std::vector<int>& log, const Pair& p) : a(next(log, p.first)), b(p.second * 5) {}
};

Pair swap_pair(const Pair& p) {
    return Pair(p.second, p.first);
}

int main() {
    Pair p(3, 9);
    Pair q{20, 30};
    Pair r = Pair(-4, 4);
    Pair s = swap_pair(Pair(7, 70));
    std::printf("%d %d | %d %d | %d %d | %d %d\n", p.first, p.second, q.first, q.second, r.first, r.second,
                s.first, s.second);
    Named n(std::string("bolt"), std::string("b2"), 6LL);
    Named m = Named(std::string("nut"), std::string("n2"), -2LL);
    std::printf("%s/%s %lld %s/%s %lld\n", n.name.c_str(), n.copy.c_str(), n.score(), m.name.c_str(),
                m.copy.c_str(), m.score());
    std::vector<int> log;
    Logged l(log, p);
    Logged k = Logged(log, Pair(8, 80));
    std::printf("%d %d %d %d log=%zu:%d,%d\n", l.a, l.b, k.a, k.b, log.size(), log[0], log[1]);
    std::vector<Pair> ps = {p, Pair(1, 2)};
    ps.push_back(Pair(11, 12));
    int acc = 0;
    for (const Pair& x : ps) {
        acc = acc * 7 + x.first - x.second;
    }
    std::printf("acc=%d\n", acc);
    return 0;
}
