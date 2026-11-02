// FR-254 R3's guards under --normalize, each a LOCATED refusal at the `++`
// naming why moving the side effect to the end of the statement could be
// observed -- never a silently different program. Another mention of the
// incremented variable keeps FR-259's own wording; a closure call can reach
// it without naming it (measured on `x++ + peek()`, `peek` reading `x` by
// reference: clang++ and g++ -O1 both print 203.0, the unguarded rewrite
// 103.0); a site under `?:` / `&&` / `||` might not run; an operand that is
// not `x` / `*p` and a non-scalar `return` have no rule. A prefix `++` used
// as a value, two increments in one statement and an increment in a
// condition are not R3's: they keep the importer's own wording. The OFF run
// pins that without --normalize the R3 sites keep the pre-R3 refusal.
// RUN: not %emitrust-bx --normalize %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not grep -q "note: normalized" %t.err
// RUN: not test -e %t.rs
// RUN: not %emitrust-bx %s 2>&1 | FileCheck --check-prefix=OFF %s
// COUNT: {{^}}10{{$}}
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

double Peek(double x) {
    auto peek = [&x]() {
        return x * 100.0;
    };
    // CHECK: normalize-incdec-guards.cpp:[[@LINE+2]]:16: error: unsupported: increment/decrement of 'x' used as a value in a statement that calls a closure or a function through a variable (normalize post-incdec: the call may read or modify 'x')
    // OFF: normalize-incdec-guards.cpp:[[@LINE+1]]:16: error: unsupported: increment/decrement used as a value
    double s = x++ + peek();
    return s + x;
}

void Guards(int* d, std::size_t* p, int n) {
    // CHECK-NEXT: normalize-incdec-guards.cpp:[[@LINE+2]]:11: error: unsupported: increment/decrement of 'p' used as a value in a statement that also uses 'p' (C++ leaves the order unsequenced or unspecified)
    // OFF-NEXT: normalize-incdec-guards.cpp:[[@LINE+1]]:11: error: unsupported: increment/decrement used as a value
    n = d[(*p)++] + d[*p];
    // CHECK-NEXT: normalize-incdec-guards.cpp:[[@LINE+2]]:7: error: unsupported: increment/decrement of 'd' used as a value in a statement that also uses 'd' (C++ leaves the order unsequenced or unspecified)
    // OFF-NEXT: normalize-incdec-guards.cpp:[[@LINE+1]]:7: error: unsupported: increment/decrement used as a value
    *(d++) = d[0];
    // CHECK-NEXT: normalize-incdec-guards.cpp:[[@LINE+2]]:19: error: unsupported: increment/decrement used as a value inside `&&`, `||`, `?:` or `,` (normalize post-incdec: it might not be evaluated)
    // OFF-NEXT: normalize-incdec-guards.cpp:[[@LINE+1]]:19: error: unsupported: increment/decrement used as a value
    n = n > 0 ? d[(*p)++] : 0;
    // CHECK-NEXT: normalize-incdec-guards.cpp:[[@LINE+2]]:20: error: unsupported: increment/decrement used as a value inside `&&`, `||`, `?:` or `,` (normalize post-incdec: it might not be evaluated)
    // OFF-NEXT: normalize-incdec-guards.cpp:[[@LINE+1]]:20: error: unsupported: increment/decrement used as a value
    n = n > 0 && d[(*p)++] > 0;
    // CHECK-NEXT: normalize-incdec-guards.cpp:[[@LINE+2]]:9: error: unsupported: increment/decrement used as a value on something other than a local variable or `*p` (normalize post-incdec: no rule)
    // OFF-NEXT: normalize-incdec-guards.cpp:[[@LINE+1]]:9: error: unsupported: increment/decrement used as a value
    n = d[0]++;
    // not R3's -- a prefix increment, two increments, a condition: the importer's wording
    // CHECK-NEXT: normalize-incdec-guards.cpp:[[@LINE+1]]:7: error: unsupported: increment/decrement used as a value
    d[++(*p)] = 1;
    // CHECK-NEXT: normalize-incdec-guards.cpp:[[@LINE+1]]:7: error: unsupported: increment/decrement used as a value
    d[(*p)++] = d[(*p)++];
    // CHECK-NEXT: normalize-incdec-guards.cpp:[[@LINE+1]]:11: error: unsupported: increment/decrement used as a value
    if (d[(*p)++] > 0) {
        n = 1;
    }
    d[0] = n;
}

std::string Name(const std::vector<std::string>& names, std::size_t* p) {
    // CHECK-NEXT: normalize-incdec-guards.cpp:[[@LINE+2]]:18: error: unsupported: increment/decrement used as a value in a `return` of type 'std::string' (normalize post-incdec: the temporary must be a scalar)
    // OFF: normalize-incdec-guards.cpp:[[@LINE+1]]:18: error: unsupported: increment/decrement used as a value
    return names[(*p)++];
}
// CHECK-NOT: error:
