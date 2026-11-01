// FR-259: an increment / decrement used as a VALUE is in the subset only on
// an integer variable of an arithmetic type that its statement mentions
// nowhere else. Every neighbour is a LOCATED refusal: `i++ + i` (unsequenced
// -- undefined -- in C++), `v[i] = i++` / `i = i++` (defined only since C++17,
// and only by the assignment rule), `printf(.., i--, i)` (argument order
// unspecified), a `?:` condition beside an increment; an unsigned compound
// store through an incrementing index (Rust's `v[i] = v[i].wrapping_add(1)`
// would evaluate the index -- and increment -- TWICE: a silent miscompile);
// the increment of a counting-loop variable or bound inside its body (C++
// re-reads `j < n` every iteration, Rust's range does not); an increment of
// a variable a live mutable pointer aliases (R2); and the value of `c++` on a
// char, `d--` on a double, `p++` on a pointer, `++u` on an unsigned char
// (the old wording: promotion and conversion back have no block spelling).
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}13{{$}}
#include <cstddef>
#include <cstdio>
#include <vector>

int seq(int i, std::vector<int>& v, std::vector<unsigned>& w, std::size_t k) {
    // CHECK:      incdec-value.cpp:[[@LINE+1]]:13: error: unsupported: increment/decrement of 'i' used as a value in a statement that also uses 'i' (C++ leaves the order unsequenced or unspecified)
    int a = i++ + i;
    // CHECK-NEXT: incdec-value.cpp:[[@LINE+1]]:38: error: unsupported: increment/decrement of 'i' used as a value in a statement that also uses 'i' (C++ leaves the order unsequenced or unspecified)
    v[static_cast<std::size_t>(i)] = i++;
    // CHECK-NEXT: incdec-value.cpp:[[@LINE+1]]:9: error: unsupported: increment/decrement of 'i' used as a value in a statement that also uses 'i' (C++ leaves the order unsequenced or unspecified)
    i = i++;
    // CHECK-NEXT: incdec-value.cpp:[[@LINE+1]]:28: error: unsupported: increment/decrement of 'i' used as a value in a statement that also uses 'i' (C++ leaves the order unsequenced or unspecified)
    std::printf("%d %d\n", i--, i);
    // CHECK-NEXT: incdec-value.cpp:[[@LINE+1]]:31: error: unsupported: increment/decrement of 'i' used as a value in a statement that also uses 'i' (C++ leaves the order unsequenced or unspecified)
    int b = (i > 0 ? i : 0) + --i;
    // CHECK-NEXT: incdec-value.cpp:[[@LINE+1]]:5: error: unsupported: unsigned compound assignment to an element whose index increments or decrements a variable
    w[k++] += 1U;
    return a + b;
}

int loops(int n) {
    int s = 0;
    // CHECK-NEXT: incdec-value.cpp:[[@LINE+1]]:5: error: unsupported: counting loop body assigns the loop variable 'j'
    for (int j = 0; j < 3; ++j) {
        s += j++;
    }
    // CHECK-NEXT: incdec-value.cpp:[[@LINE+1]]:5: error: unsupported: counting loop body assigns or shadows the bound 'n'
    for (int j = 0; j < n; ++j) {
        s += n-- > 1 ? 1 : 0;
    }
    return s;
}

int alias(int x) {
    int* p = &x;
    // CHECK-NEXT: incdec-value.cpp:[[@LINE+1]]:13: error: unsupported: 'x' is used while 'p', a mutable pointer derived from it, is in scope (a second borrow in Rust)
    int y = x++ + *p;
    return y;
}

void kinds(char c, double d, int* p, unsigned char u, bool f) {
    // CHECK-NEXT: incdec-value.cpp:[[@LINE+1]]:15: error: unsupported: increment/decrement used as a value
    char c2 = c++;
    // CHECK-NEXT: incdec-value.cpp:[[@LINE+1]]:17: error: unsupported: increment/decrement used as a value
    double d2 = d--;
    // CHECK-NEXT: incdec-value.cpp:[[@LINE+1]]:14: error: unsupported: increment/decrement used as a value
    int* q = p++;
    // CHECK-NEXT: incdec-value.cpp:[[@LINE+1]]:24: error: unsupported: increment/decrement used as a value
    unsigned char u2 = ++u;
    std::printf("%d %f %d %d\n", c2, d2, *q, u2);
}

int main() {
    return 0;
}
