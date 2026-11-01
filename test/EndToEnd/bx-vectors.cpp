// REQUIRES: cargo
// FR-251: emitrust-bx C++ -> Rust, the std::vector rows end to end (byte-diff
// vs clang++). Pins every construction form (`std::vector<T> v;`, `= {..}`,
// `(n, x)`, nested `{{..}, {..}}`) <=> Vec::new / vec![..] / vec![x; n],
// push_back/pop_back/clear/resize, size/empty, `v[i]` read, write and
// compound write (`++v[i]`, `v[i] *= k`) with an `int` index (the Kernel's
// explicit cast makes it `v[i as usize]`), front()/back() (<=> `v[0]` /
// `v[v.len() - 1]`), ==/!= on vectors, a vector of strings, and a counting
// loop bounded by `v.size()` whose body leaves v's size alone. UB-free:
// every index is in range, pop_back/back/front only on non-empty vectors.
// Distinct values at every call site so an argument wired to the wrong
// parameter shows.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_vectors > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>
#include <string>
#include <vector>

std::vector<long long> squares(int n, long long bias) {
    std::vector<long long> out;
    for (int i = 0; i < n; ++i) {
        out.push_back(static_cast<long long>(i) * i + bias);
    }
    return out;
}

long long dot(const std::vector<long long>& a, const std::vector<long long>& b) {
    long long acc = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        acc += a[i] * b[i];
    }
    return acc;
}

int checksum(const std::vector<int>& v) {
    int h = 17;
    for (std::size_t i = 0; i < v.size(); ++i) {
        h = h * 31 % 1000003 + v[i];
    }
    return h;
}

void show(const std::vector<int>& v) {
    std::printf("[");
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i > 0) {
            std::printf(" ");
        }
        std::printf("%d", v[i]);
    }
    std::printf("] n=%zu empty=%d\n", v.size(), v.empty());
}

int main() {
    std::vector<int> a = {5, -3, 12, 7};
    std::vector<int> b(3, 9);
    std::vector<int> c;
    show(a);
    show(b);
    show(c);
    a.push_back(40);
    a.push_back(-1);
    a.pop_back();
    show(a);
    int k = 2;
    a[k] = a[k] * 10;
    a[0] += 100;
    ++a[1];
    a[3] *= -4;
    show(a);
    std::printf("front=%d back=%d checksum=%d\n", a.front(), a.back(), checksum(a));
    b.resize(6, -8);
    show(b);
    b.resize(2, 77);
    show(b);
    b.clear();
    show(b);
    std::vector<long long> s1 = squares(5, 3);
    std::vector<long long> s2 = squares(5, -2);
    std::printf("dot=%lld last=%lld\n", dot(s1, s2), s2.back());
    std::vector<int> d = {5, -3, 12, 7};
    std::vector<int> e = {5, -3, 12};
    std::printf("eq=%d ne=%d eq2=%d\n", a == d, d != e, d == a);
    e.push_back(7);
    std::printf("eq3=%d\n", d == e);
    std::vector<std::vector<int>> grid = {{1, 2, 3}, {4}, {}};
    grid[2].push_back(9);
    const int six = grid[0][2] * 6;
    grid[1].push_back(six);
    grid.push_back(std::vector<int>(2, 11));
    for (std::size_t r = 0; r < grid.size(); ++r) {
        show(grid[r]);
    }
    std::vector<std::string> words = {"alpha", "be"};
    words.push_back("gamma");
    words[1] += "ta";
    std::printf("%zu %s %s %s %zu\n", words.size(), words[0].c_str(), words[1].c_str(), words.back().c_str(),
                words[2].size());
    std::vector<unsigned char> bytes(4, 250);
    bytes[1] = 3;
    unsigned total = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        total += bytes[i];
    }
    std::printf("bytes total=%u\n", total);
    return 0;
}
