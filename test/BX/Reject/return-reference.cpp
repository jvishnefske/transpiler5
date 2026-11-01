// FR-251: references are PARAMETERS (and range-for variables) only. A
// function returning a reference is the nearest out-of-subset neighbour --
// its Rust spelling needs lifetime annotations the subset does not model --
// and a local reference variable is not in the subset either. Each is ONE
// located residual: the returns inside a rejected function are cascades,
// not counted again.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}3{{$}}
#include <string>
#include <vector>

// CHECK: return-reference.cpp:[[@LINE+1]]:25: error: unsupported: function returning a reference
const std::vector<int>& longest(const std::vector<int>& a, const std::vector<int>& b) {
    if (a.size() >= b.size()) {
        return a;
    }
    return b;
}

// CHECK-NEXT: return-reference.cpp:[[@LINE+1]]:14: error: unsupported: function returning a reference
std::string& itself(std::string& s) {
    return s;
}

int main() {
    std::vector<int> x = {1};
    // CHECK-NEXT: return-reference.cpp:[[@LINE+1]]:5: error: unsupported: variable of type 'int &'
    int& r = x[0];
    return 0;
}
