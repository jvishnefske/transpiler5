// REQUIRES: cargo
// FR-251: emitrust-bx C++ -> Rust, copy vs move end to end (byte-diff vs
// clang++). The mapping is SEMANTIC, read from clang's AST: a copy
// constructor / copy assignment / `push_back(const T&)` <=> `.clone()`, a
// move (`std::move(x)` binding `T&&`) <=> a bare `x`, and `return v;` of a
// local (C++17 implicit move) <=> `return v;`. The observable half: every
// COPY is independent afterwards -- mutating it must not change the
// original, in either language -- while moved-from variables are never
// mentioned again (the subset rejects that). Also pins by-value vector and
// string parameters (`f(v)` copies: `f(v.clone())`; `f(std::move(v))`
// moves), copy-assignment over an existing vector, a vector of vectors
// filled by moves, and `std::vector<std::string>(n, s)` copying s n times.
// UB-free; distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_moves > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

std::vector<int> with_tail(std::vector<int> v, int tail) {
    v.push_back(tail);
    v[0] = v[0] * 2;
    return v;
}

std::string shouted(std::string s) {
    s += "!!";
    return s;
}

int first_plus_size(std::vector<int> v) {
    return v[0] + static_cast<int>(v.size());
}

void show(const std::vector<int>& v) {
    std::printf("[");
    for (int x : v) {
        std::printf(" %d", x);
    }
    std::printf(" ]\n");
}

int main() {
    std::vector<int> base = {1, 2, 3};
    std::vector<int> copy = base;
    copy[0] = 100;
    copy.push_back(4);
    show(base);
    show(copy);
    std::vector<int> grown = with_tail(base, 9);
    show(base);
    show(grown);
    std::vector<int> moved_in = with_tail(std::move(copy), -7);
    show(moved_in);
    std::printf("fps=%d %d\n", first_plus_size(base), first_plus_size(grown));
    std::vector<int> target = {50, 60};
    target = grown;
    grown[1] = -1;
    show(target);
    show(grown);
    target = std::move(grown);
    show(target);
    std::string s = "hey";
    std::string t = shouted(s);
    std::string u = shouted(std::move(t));
    std::printf("%s %s\n", s.c_str(), u.c_str());
    std::vector<std::string> names;
    names.push_back(s);
    s += "-changed";
    names.push_back(std::move(u));
    names.push_back(s + "+");
    std::printf("%s|%s|%s|%s\n", names[0].c_str(), names[1].c_str(), names[2].c_str(), s.c_str());
    std::vector<std::string> fill(3, s);
    fill[1] += "X";
    std::printf("%s %s %s\n", fill[0].c_str(), fill[1].c_str(), fill[2].c_str());
    std::vector<std::vector<int>> rows;
    for (int r = 0; r < 3; ++r) {
        std::vector<int> row(static_cast<std::size_t>(r + 1), r * 10);
        row.push_back(-r);
        rows.push_back(std::move(row));
    }
    std::vector<int> snapshot = rows[1];
    rows[1].push_back(77);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        show(rows[i]);
    }
    show(snapshot);
    return 0;
}
