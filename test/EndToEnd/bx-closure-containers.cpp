// REQUIRES: cargo
// FR-252: emitrust-bx C++ -> Rust, closures over NON-SCALAR captures end to
// end (byte-diff vs clang++). A by-reference capture of a vector / string /
// struct is a borrow (the closure pushes, appends, writes members and calls
// a non-const member function; the owner sees every change once the closure
// is dead). A BY-COPY capture of one is C++'s copy and Rust's `move`, equal
// only because the variable is never mentioned after the closure is made --
// the copy-vs-move rows here are exactly those. Also a by-reference capture
// of a reference parameter and of a range-for element reference.
// UB-free; distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_closure_containers > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>
#include <string>
#include <vector>

struct Tally {
    int count;
    long long sum;
    void add(int x);
    long long mean() const;
};

void Tally::add(int x) {
    count += 1;
    sum += static_cast<long long>(x);
}

long long Tally::mean() const {
    return count == 0 ? 0LL : sum / static_cast<long long>(count);
}

std::size_t grow(int seed) {
    std::vector<int> v = {seed};
    auto push = [&v](int x) -> std::size_t {
        v.push_back(x * 2);
        return v.size();
    };
    std::size_t a = push(3);
    std::size_t b = push(5);
    std::size_t c = push(8);
    std::printf("grow: %zu %zu %zu last=%d first=%d\n", a, b, c, v[v.size() - 1], v[0]);
    return v.size();
}

int text(int n) {
    std::string s = "ab";
    auto app = [&s](char c, int times) {
        for (int i = 0; i < times; ++i) {
            s.push_back(c);
        }
        s += "|";
    };
    app('x', n);
    app('y', n + 1);
    std::printf("text: %s (%zu)\n", s.c_str(), s.size());
    return static_cast<int>(s.size());
}

long long stats(int a, int b) {
    Tally t = {0, 0LL};
    auto feed = [&t](int x) {
        t.add(x);
        t.add(x + 1);
    };
    feed(a);
    feed(b);
    std::printf("stats: count=%d sum=%lld mean=%lld\n", t.count, t.sum, t.mean());
    return t.sum;
}

int owned_copy(int seed) {
    std::vector<int> w = {seed, seed + 1, seed + 2};
    std::string name = "copy";
    auto peek = [w, name](std::size_t i) -> int {
        return w[i] * 100 + static_cast<int>(name.size());
    };
    std::printf("owned: %d %d %d\n", peek(0), peek(1), peek(2));
    return peek(2);
}

int owned_mutable(int seed) {
    std::vector<int> q = {seed};
    auto acc = [q](int x) mutable -> int {
        q.push_back(x);
        int total = 0;
        for (int e : q) {
            total += e;
        }
        return total;
    };
    int a = acc(10);
    int b = acc(20);
    std::printf("owned_mutable: %d %d\n", a, b);
    return b;
}

void bump_all(std::vector<int>& xs, int by) {
    auto bump = [&xs](std::size_t i, int d) {
        xs[i] += d;
    };
    for (std::size_t i = 0; i < 3; ++i) {
        bump(i, by * static_cast<int>(i + 1));
    }
}

int elements(int seed) {
    std::vector<int> xs = {seed, 2 * seed, 3 * seed};
    bump_all(xs, 7);
    for (int& e : xs) {
        auto twice = [&e]() {
            e = e * 2;
        };
        twice();
        twice();
    }
    std::printf("elements: %d %d %d\n", xs[0], xs[1], xs[2]);
    return xs[2];
}

int main() {
    std::size_t g = grow(9);
    int t = text(2);
    long long s = stats(4, 10);
    int o = owned_copy(6);
    int m = owned_mutable(3);
    int e = elements(5);
    std::printf("main: %zu %d %lld %d %d %d\n", g, t, s, o, m, e);
    return 0;
}
