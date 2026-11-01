// FR-251: `std::move(x)` <=> a bare `x` holds only when x is NEVER mentioned
// again. The nearest neighbours are LOCATED residuals, never a rustc E0382
// on accepted input and never a read of a moved-from C++ object: a use after
// the move (C++: valid-but-unspecified, Rust: a compile error), a move
// inside a loop of a variable declared outside it (the second iteration),
// std::move of a const object (C++ silently COPIES -- clang picks the copy
// constructor, which the importer reads), and a move out of a reference
// parameter (Rust cannot move out of `&mut`). Every residual is reported.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}4{{$}}
#include <cstdio>
#include <utility>
#include <vector>

std::vector<int> take(std::vector<int>& v) {
    // CHECK: use-after-move.cpp:[[@LINE+1]]:12: error: unsupported: move out of reference 'v' (the subset copies it: C++ copy, Rust `.clone()`)
    return std::move(v);
}

int main() {
    std::vector<int> a = {1, 2};
    // CHECK-NEXT: use-after-move.cpp:[[@LINE+1]]:26: error: unsupported: 'a' is used after it is moved (std::move(a) <=> a requires no later mention of a)
    std::vector<int> b = std::move(a);
    a.push_back(3);
    const std::vector<int> c = {4};
    // CHECK-NEXT: use-after-move.cpp:[[@LINE+1]]:26: error: unsupported: std::move of a const object (it copies)
    std::vector<int> d = std::move(c);
    std::vector<int> e = {5};
    std::vector<std::vector<int>> all;
    for (int i = 0; i < 2; ++i) {
        // CHECK-NEXT: use-after-move.cpp:[[@LINE+1]]:23: error: unsupported: move of 'e' inside a loop it was declared outside of
        all.push_back(std::move(e));
    }
    std::printf("%zu %zu %zu\n", b.size(), d.size(), all.size());
    return 0;
}
