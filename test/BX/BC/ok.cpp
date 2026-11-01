// FR-255: bc.hpp's POSITIVE program -- what the borrow discipline must ACCEPT,
// compiled with -std=c++20 and run. Pins: two Owners of the same T are distinct
// types, so disjoint mutable borrows of them compose in one bc::with; shared +
// shared of one Owner is fine; owners nest inside std::pair (composition of
// existing types), and a reborrow of the inner owner inside the outer borrow
// works; BC_BORROW_FREE opts a user aggregate in; clone() makes a new value;
// a borrow-free result (int, size_t) leaves the callback. Each accepted form
// works; bc::with_two borrows two distinct elements of a std::vector of
// Owners mutably at once (the runtime-checked split the type-level check
// refuses). Each accepted form has a rejected neighbour in reject.cpp, and
// with_two's aliasing call aborts in with-two-abort.cpp.
// RUN: clang++ -std=c++20 -I%bx_src/include %s -o %t
// RUN: %t | FileCheck %s
// CHECK: 9 41 3 [1 2 3] [1 2 3]
// CHECK-NEXT: 20 10 30
#include "bc.hpp"
#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

struct Point {
    int x, y;
};
BC_BORROW_FREE(Point, int, int);

int main() {
    bc::Owner<std::vector<int>> a(std::vector<int>{3, 1, 2});
    bc::Owner<std::vector<int>> b;  // same T, DIFFERENT type
    static_assert(!std::is_same_v<decltype(a), decltype(b)>);
    bc::with(a, bc::mut(b), [](auto& x, auto& y) {  // two disjoint mutable borrows
        std::sort(x.begin(), x.end());
        y = x;
    });
    int sum = bc::with(bc::ref(a), bc::ref(a), [](const auto& p, const auto& q) {
        int s = 0;
        for (int v : p) s += v;
        return s + static_cast<int>(q.size());
    });
    using P = bc::Owner<Point>;  // name a nested owner type ONCE
    bc::Owner<std::pair<P, std::string>> nested(P(Point{1, 2}), "hi");
    bc::with(nested, [](auto& pr) {
        bc::with(pr.first, [](Point& p) { p.x += 40; });  // reborrow inside the outer borrow
    });
    int px = bc::with(bc::ref(nested), [](const auto& pr) {
        return bc::with(bc::ref(pr.first), [](const Point& p) { return p.x; });
    });
    auto c = a.clone();
    std::size_t n = bc::with(bc::ref(c), [](const auto& v) { return v.size(); });
    std::printf("%d %d %zu", sum, px, n);
    bc::with(bc::ref(a), bc::ref(b), [](const auto& x, const auto& y) {
        std::printf(" [%d %d %d] [%d %d %d]\n", x[0], x[1], x[2], y[0], y[1], y[2]);
    });
    using I = bc::Owner<int>;
    std::vector<I> v;
    v.emplace_back(10);
    v.emplace_back(20);
    v.emplace_back(30);
    bc::with_two(v, 1, 0, [](int& p, int& q) { std::swap(p, q); });
    std::printf("%d %d %d\n", bc::with(bc::ref(v[0]), [](const int& x) { return x; }),
                bc::with(bc::ref(v[1]), [](const int& x) { return x; }),
                bc::with(bc::ref(v[2]), [](const int& x) { return x; }));
    return 0;
}
