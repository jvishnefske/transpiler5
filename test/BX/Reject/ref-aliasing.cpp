// FR-251: the conservative alias check for reference parameters. `f(v, v)`
// with at least one `T&` would be two live borrows of v in Rust (E0502) and
// is self-aliasing in C++; `bump(k) + k` reads k in the same statement
// that a call modifies it -- C++ leaves that order UNSPECIFIED while Rust
// fixes it left to right, so no single translation is faithful. Both are
// LOCATED residuals. The distinct-argument call and the call alone in its
// statement stay in the subset.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}2{{$}}
#include <cstdio>
#include <vector>

void merge(std::vector<int>& into, const std::vector<int>& from) {
    for (std::size_t i = 0; i < from.size(); ++i) {
        into.push_back(from[i]);
    }
}

int bump(int& k) {
    ++k;
    return k;
}

int main() {
    std::vector<int> v = {1};
    std::vector<int> w = {2};
    merge(v, w);
    // CHECK: ref-aliasing.cpp:[[@LINE+1]]:5: error: unsupported: 'v' is passed to 'merge' by non-const reference and also used by another argument of the same call (aliasing)
    merge(v, v);
    int k = 1;
    // CHECK-NEXT: ref-aliasing.cpp:[[@LINE+1]]:13: error: unsupported: 'k' is passed to 'bump' by non-const reference and also used elsewhere in the statement (C++ leaves the order unspecified)
    int r = bump(k) + k;
    int s = bump(k);
    std::printf("%zu %d %d %d\n", v.size(), k, r, s);
    return 0;
}
