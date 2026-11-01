// FR-255: bc::with_two discharges at RUN time what the type-level check
// refuses (two mutable borrows of same-typed elements), so its check must
// never hand out two references to ONE element: i == j aborts loudly (Rust's
// get_disjoint_mut / split_at_mut would panic), before the callback runs. The
// out-of-range case takes the same path.
// RUN: clang++ -std=c++20 -I%bx_src/include %s -o %t
// RUN: not --crash %t 2>&1 | FileCheck %s
// CHECK: bc::with_two: indices 1 and 1 of a vector of 2 do not name two distinct elements
// CHECK-NOT: callback ran
#include "bc.hpp"
#include <cstdio>
#include <vector>

int main() {
    std::vector<int> v = {1, 2};
    bc::with_two(v, 1, 1, [](int& a, int& b) {
        std::printf("callback ran\n");
        a += b;
    });
    return 0;
}
