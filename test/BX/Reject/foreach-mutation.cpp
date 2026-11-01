// FR-251: the range-for alias check. A body that modifies the vector it
// iterates (push_back invalidates C++'s iterators; Rust's `.iter()` borrow
// forbids it -- even an element write, harmless in C++, conflicts with that
// borrow), or that so much as mentions it under `for (T& x : v)`
// (`.iter_mut()` holds the only borrow), is a LOCATED residual -- never a
// rustc E0502 on accepted input. Same for a counting loop bounded by
// `v.size()` whose body changes v's SIZE (C++ re-reads the bound every
// iteration, Rust's `0..v.len()` once). The last loop writes ELEMENTS only:
// the size is unchanged, so it stays in the subset (no diagnostic).
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}4{{$}}
#include <cstdio>
#include <vector>

int main() {
    std::vector<int> v = {1, 2, 3};
    // CHECK: foreach-mutation.cpp:[[@LINE+1]]:5: error: unsupported: the body of a range-for over 'v' modifies 'v' (iterator invalidation in C++, a borrow conflict in Rust)
    for (int x : v) {
        v.push_back(x);
    }
    // CHECK-NEXT: foreach-mutation.cpp:[[@LINE+1]]:5: error: unsupported: the body of a range-for over 'v' modifies 'v' (iterator invalidation in C++, a borrow conflict in Rust)
    for (const int& x : v) {
        v[0] = x;
    }
    // CHECK-NEXT: foreach-mutation.cpp:[[@LINE+1]]:5: error: unsupported: the body of a mutating range-for over 'v' uses 'v' (iterator invalidation in C++, a second borrow in Rust)
    for (int& x : v) {
        x += static_cast<int>(v.size());
    }
    // CHECK-NEXT: foreach-mutation.cpp:[[@LINE+1]]:5: error: unsupported: counting loop body modifies or shadows 'v', whose size is the bound
    for (std::size_t i = 0; i < v.size(); ++i) {
        v.pop_back();
    }
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = 7;
    }
    std::printf("%zu\n", v.size());
    return 0;
}
