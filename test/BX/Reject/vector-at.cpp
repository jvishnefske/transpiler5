// FR-251: `v[i]` is in the subset; `v.at(i)` -- the nearest neighbour -- is
// a LOCATED residual: it is bounds-checked and THROWS std::out_of_range,
// and the subset has no exceptions (Rust's `v[i]` panics on the same
// indices, but a catch elsewhere would observe the difference). Read and
// write uses are each reported.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}2{{$}}
#include <cstdio>
#include <vector>

int main() {
    std::vector<int> v = {1, 2, 3};
    int a = v[1];
    // CHECK: vector-at.cpp:[[@LINE+1]]:13: error: unsupported: std::vector::at (bounds-checked access that throws; the subset has operator[])
    int b = v.at(2);
    // CHECK-NEXT: vector-at.cpp:[[@LINE+1]]:5: error: unsupported: std::vector::at (bounds-checked access that throws; the subset has operator[])
    v.at(0) = 5;
    std::printf("%d %d\n", a, b);
    return 0;
}
