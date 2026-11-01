// FR-257 (closes an FR-251 miscompile): a counting loop bounded by
// `x.size()` re-reads the bound every iteration in C++ while Rust's
// `0..x.len()` reads it once, so the subset requires that the body never
// changes the bound's SIZE. FR-251 checked this only when the bound was a
// bare variable: `i < v[0].size()` with `v[0].push_back(..)` (or `grow(v[1])`
// through `std::vector<int>&`) in the body was ACCEPTED, and the emitted Rust
// printed `4 2` where clang++'s binary prints `5 3`. The rule is now stated
// over access paths (a mutation reaches the bound when its target is a prefix
// of it, every index a wildcard), so these and the struct-member bound
// `b.items.size()` with `b.items.push_back(..)` are located residuals, while
// writing elements and other members (`b.items[i] += 1; b.n += 1;`) under the
// same bound stays in the subset.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}3{{$}}
#include <cstdio>
#include <vector>

struct Bag {
    std::vector<int> items;
    int n;
};

void grow(std::vector<int>& x) {
    x.push_back(1);
}

int main() {
    std::vector<std::vector<int>> v = {{1, 2}, {3}};
    // CHECK: len-bound-paths.cpp:[[@LINE+1]]:5: error: unsupported: counting loop body modifies or shadows 'v', whose size is the bound
    for (std::size_t i = 0; i < v[0].size(); ++i) {
        if (i < 3) {
            v[0].push_back(5);
        }
    }
    // CHECK-NEXT: len-bound-paths.cpp:[[@LINE+1]]:5: error: unsupported: counting loop body modifies or shadows 'v', whose size is the bound
    for (std::size_t i = 0; i < v[1].size(); ++i) {
        if (i < 2) {
            grow(v[1]);
        }
    }
    Bag b = {{4, 5}, 0};
    for (std::size_t i = 0; i < b.items.size(); ++i) {
        b.items[i] += 1;
        b.n += 1;
    }
    // CHECK-NEXT: len-bound-paths.cpp:[[@LINE+1]]:5: error: unsupported: counting loop body modifies or shadows 'b', whose size is the bound
    for (std::size_t i = 0; i < b.items.size(); ++i) {
        if (i < 2) {
            b.items.push_back(7);
        }
    }
    std::printf("%zu %zu %d\n", v[0].size(), v[1].size(), b.n);
    return 0;
}
