// REQUIRES: cargo
// FR-251: emitrust-bx C++ -> Rust, references and range-for end to end
// (byte-diff vs clang++). Pins `const std::vector<T>&` <=> `&Vec<T>`,
// `std::vector<T>&` <=> `&mut Vec<T>`, `const std::string&` <=> `&String`,
// `std::string&` <=> `&mut String`, scalar `int&` / `const long long&` <=>
// `&mut i32` / `&i64` (`*x` in Rust), call sites `f(v)` <=> `f(&v)` /
// `f(&mut v)`, a reference parameter forwarded to another reference
// parameter (bare in Rust: auto-reborrow), an element passed by `T&`, and
// all three range-for modes: `for (T x : v)` <=> `.iter().copied()` (and
// `.cloned()` for strings, `for mut x` when the C++ copy is modified),
// `for (const T& x : v)` <=> `.iter()`, `for (T& x : v)` <=> `.iter_mut()`.
// The alias rules keep every accepted call borrow-clean: no variable is
// passed by non-const reference twice or alongside another use of itself.
// UB-free; distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_refs > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>
#include <string>
#include <vector>

void bump(int& x, const long long& by) {
    x += static_cast<int>(by);
}

void add_all(std::vector<int>& v, int delta) {
    for (int& x : v) {
        x += delta;
    }
}

long long sum(const std::vector<int>& v) {
    long long s = 0;
    for (const int& x : v) {
        s += x;
    }
    return s;
}

int max_of(const std::vector<int>& v) {
    int best = v[0];
    for (int x : v) {
        if (x > best) {
            best = x;
        }
    }
    return best;
}

void twice(std::vector<int>& v, int delta) {
    add_all(v, delta);
    add_all(v, delta + 1);
    v.push_back(static_cast<int>(sum(v)));
}

void decorate(std::string& s, const std::string& left, const std::string& right) {
    std::string tmp = left;
    tmp += s;
    tmp += right;
    s = tmp;
}

std::size_t total_len(const std::vector<std::string>& words) {
    std::size_t n = 0;
    for (const std::string& w : words) {
        n += w.size();
    }
    return n;
}

void shout(std::vector<std::string>& words) {
    for (std::string& w : words) {
        w += "!";
    }
}

std::string joined(const std::vector<std::string>& words) {
    std::string out;
    for (std::string w : words) {
        w += ";";
        out += w;
    }
    return out;
}

int main() {
    int a = 10;
    const long long by = 32;
    bump(a, by);
    bump(a, -5);
    std::printf("a=%d\n", a);
    std::vector<int> v = {3, -1, 4, 1, -5};
    std::printf("sum=%lld max=%d\n", sum(v), max_of(v));
    add_all(v, 7);
    std::printf("sum=%lld max=%d\n", sum(v), max_of(v));
    twice(v, -2);
    std::printf("sum=%lld max=%d n=%zu back=%d\n", sum(v), max_of(v), v.size(), v.back());
    bump(v[1], 1000);
    std::printf("v1=%d\n", v[1]);
    std::vector<int> w = {8, 6};
    add_all(w, 2);
    std::printf("w=%lld v=%lld\n", sum(w), sum(v));
    std::string name = "core";
    const std::string l = "[[";
    decorate(name, l, "]]");
    decorate(name, "<", ">");
    std::printf("%s %zu\n", name.c_str(), name.size());
    std::vector<std::string> words = {"red", "green", "blue"};
    std::printf("%zu %s\n", total_len(words), joined(words).c_str());
    shout(words);
    std::printf("%zu %s %s\n", total_len(words), joined(words).c_str(), words[2].c_str());
    int hits = 0;
    for (const std::string& x : words) {
        if (x.size() > 4) {
            ++hits;
        }
    }
    std::printf("hits=%d\n", hits);
    return 0;
}
