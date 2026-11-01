// FR-251: an element WRITE whose index reads the same vector --
// `v.back() = x` (which desugars to `v[v.size() - 1]`), `v[v.size() - 2] +=
// 1` -- and a mutating call on an element whose argument reads the same
// vector (`g[0].push_back(g[1][0])`) are LOCATED residuals: Rust takes
// `&mut v` for IndexMut BEFORE evaluating the index / argument, so the
// read is E0502 (measured with rustc 1.96, also through `&mut Vec`). Reads
// of `v.back()` and a write through a precomputed index stay in the subset.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}3{{$}}
#include <cstdio>
#include <vector>

int main() {
    std::vector<int> v = {1, 2, 3};
    std::vector<std::vector<int>> g = {{1}, {2}};
    int last = v.back();
    // CHECK: self-index-write.cpp:[[@LINE+1]]:5: error: unsupported: an element of 'v' is written through an index that reads 'v' (a second borrow in Rust)
    v.back() = 9;
    // CHECK-NEXT: self-index-write.cpp:[[@LINE+1]]:5: error: unsupported: an element of 'v' is written through an index that reads 'v' (a second borrow in Rust)
    v[v.size() - 2] += 1;
    // CHECK-NEXT: self-index-write.cpp:[[@LINE+1]]:5: error: unsupported: mutating an element of 'g' with an argument that reads 'g'
    g[0].push_back(g[1][0]);
    const std::size_t k = v.size() - 1;
    v[k] = v[0] + v.back();
    std::printf("%d %d %zu\n", last, v[k], g[0].size());
    return 0;
}
