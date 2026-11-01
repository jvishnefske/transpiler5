// FR-254: the normalizer's guards under --normalize, each a LOCATED refusal at
// the loop naming why its rewrite would be unsound -- never a silently
// different program. A `continue` bound to the rewritten loop is refused (in
// C++ it runs the `for`'s step, or jumps to the do-while's test; in the
// `while` form either would be skipped); a `continue` bound to a NESTED loop
// is fine. A `for` with a condition variable has no rule. A step that is not
// a Kernel statement (the comma operator) keeps the importer's own located
// refusal inside the rewritten loop. The OFF run pins that without
// --normalize the same loops keep the pre-FR-254 refusals byte-for-byte.
// RUN: not %emitrust-bx --normalize %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: FileCheck --check-prefix=NOTE %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// RUN: not %emitrust-bx %s 2>&1 | FileCheck --check-prefix=OFF %s
// COUNT: {{^}}4{{$}}
#include <cstdio>

int f(int n) {
    int s = 0;
    // CHECK: normalize-guards.cpp:[[@LINE+2]]:5: error: unsupported: `for` loop with a `continue` bound to it (normalize for->while: the `while` form's `continue` would skip the step)
    // OFF: normalize-guards.cpp:[[@LINE+1]]:5: error: unsupported: `for` loop outside the canonical counting form `for (int i = a; i < b; ++i)`
    for (int i = 0; i < n; i += 2) {
        if (i == 4) {
            continue;
        }
        s += i;
    }
    // CHECK-NEXT: normalize-guards.cpp:[[@LINE+2]]:5: error: unsupported: do-while loop with a `continue` bound to it (normalize do-while: the `while (true)` form's `continue` would skip the condition)
    // OFF-NEXT: normalize-guards.cpp:[[@LINE+1]]:5: error: unsupported: do-while loop
    do {
        n -= 1;
        if (n == 2) {
            continue;
        }
        s += n;
    } while (n > 0);
    // CHECK-NEXT: normalize-guards.cpp:[[@LINE+2]]:5: error: unsupported: `for` loop with a condition variable (normalize for->while: no rule)
    // OFF-NEXT: normalize-guards.cpp:[[@LINE+1]]:5: error: unsupported: `for` loop outside the canonical counting form `for (int i = a; i < b; ++i)`
    for (; int r = n - s;) {
        s += r;
    }
    // the `continue` is bound to the nested canonical `for`: this loop IS
    // rewritten, and its step's comma operator is refused inside the result
    // NOTE: normalize-guards.cpp:[[@LINE+4]]:5: note: normalized: for->while
    // NOTE-NOT: note:
    // CHECK-NEXT: normalize-guards.cpp:[[@LINE+2]]:28: error: unsupported: comma operator
    // OFF-NEXT: normalize-guards.cpp:[[@LINE+1]]:5: error: unsupported: `for` loop outside the canonical counting form `for (int i = a; i < b; ++i)`
    for (int i = 0; i < n; i += 1, s += 2) {
        for (int j = 0; j < 3; ++j) {
            if (j == 1) {
                continue;
            }
            s += j;
        }
    }
    return s;
}
