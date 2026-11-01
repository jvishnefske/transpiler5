// REQUIRES: cargo
// FR-259: emitrust-bx C++ -> Rust, `static` / `inline` free functions and
// `auto` declarations end to end (byte-diff vs clang++; normal forms N21 and
// N22). At the root `static` and `inline` are dropped -- including functions
// named like Rust prelude items that stay legal Rust function names (`drop`,
// `Box`, `vec`, `clone`); inside a namespace a `static` function is internal
// (the anonymous-namespace member, a private `fn` in the module) and is
// called by its namespace's public functions. `auto` takes clang's deduced
// type: int, `const auto` of a `std::uint8_t` element, `std::size_t` from
// `size()`, a std::vector copy (a `.clone()`), a std::string, a
// `const std::uint8_t*` from `data()`, a struct; and range-for `const auto&`,
// `auto`, `auto&` (writing through) and `auto&&` (an lvalue element: `T&`).
// UB-free; distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_auto_static > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct Pair {
    int a;
    int b;
};

static int drop(int x) {
    return x - 1;
}

static inline int Box(int x) {
    return drop(x) * 2;
}

inline int vec(int x) {
    return Box(x) + 3;
}

static int clone(int x) {
    return x * 5;
}

namespace tally {
static int weight(int v) {
    return v % 7 + 1;
}

static inline int scaled(int v) {
    return weight(v) * 10;
}

int total(const std::vector<int>& xs) {
    int s = 0;
    for (const auto& x : xs) {
        s += scaled(x);
    }
    return s;
}
}  // namespace tally

int autos(const std::vector<std::uint8_t>& v, int x) {
    auto a = x + 1;
    const auto b = v[0];
    auto n = v.size();
    auto w = v;
    w.push_back(b);
    auto s = std::string("ab");
    s += "cd";
    auto p = v.data();
    auto q = Pair{x, a};
    q.b += 4;
    return a + b + static_cast<int>(n) * 100 + static_cast<int>(w.size()) * 1000 + static_cast<int>(s.size()) + p[1] + q.b;
}

int loops(std::vector<int>& xs) {
    int s = 0;
    for (auto x : xs) {
        s += x;
    }
    for (auto& x : xs) {
        x = x * 2 + 1;
    }
    for (auto&& x : xs) {
        x += 3;
    }
    for (const auto& x : xs) {
        s = s * 10 + x % 10;
    }
    return s;
}

int main() {
    std::printf("%d %d %d %d %d\n", drop(10), Box(20), vec(30), clone(4), vec(clone(2)));
    std::vector<int> xs = {3, 11, 26};
    std::printf("%d\n", tally::total(xs));
    std::vector<std::uint8_t> v = {9, 201};
    std::printf("%d %d\n", autos(v, 41), autos(std::vector<std::uint8_t>{3, 4, 5}, -6));
    int r = loops(xs);
    std::printf("%d %d %d %d\n", r, xs[0], xs[1], xs[2]);
    return 0;
}
