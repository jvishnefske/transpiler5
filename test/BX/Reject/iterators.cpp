// FR-251: iterators are not in the subset outside the range-for that
// desugars to them (which is). `v.begin()` / `v.end()` used directly -- a
// dereference, an iterator variable, iterator arithmetic -- are LOCATED
// residuals naming the iterator call, not the operator around it.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}3{{$}}
#include <cstdio>
#include <vector>

int main() {
    std::vector<int> v = {4, 5};
    // CHECK: iterators.cpp:[[@LINE+1]]:18: error: unsupported: std::vector::begin (iterators and pointers are outside the subset)
    int first = *v.begin();
    // CHECK-NEXT: iterators.cpp:[[@LINE+1]]:5: error: unsupported: variable of type 'std::vector<int>::iterator'
    std::vector<int>::iterator it = v.begin();
    // CHECK-NEXT: iterators.cpp:[[@LINE+1]]:14: error: unsupported: std::vector::end (iterators and pointers are outside the subset)
    long n = v.end() - v.begin();
    for (int x : v) {
        first += x;
    }
    std::printf("%d %ld\n", first, n);
    return 0;
}
