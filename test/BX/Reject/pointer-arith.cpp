// FR-261: pointer ARITHMETIC outside the suffix-slice model is a LOCATED
// refusal. Pins R3 -- C++ defines a NEGATIVE offset into the middle of an
// array (the spike's Prev(arr + 3, -1) returns 30 natively, while a Rust
// suffix slice panics: a behaviour change on DEFINED input) -- so a signed
// index or offset that is not provably non-negative (`p[k]`, `p + k` with an
// `int` / `long` k), `p - k`, `--p`, `p--` and `p -= k` are refused; and R4
// -- pointer `==`, `!=`, `<`, `>=` and pointer difference `b - a` have no
// slice equivalent (suffixes of DISTINCT objects can have equal lengths, so
// comparing lengths would be a silent miscompile). `2 + p` (the offset first)
// is not the canonical `p + k`. A C array (`int v[4]`) stays outside the
// subset as before.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}14{{$}}
#include <cstdio>

int at(const int* p, int k) {
    // CHECK: pointer-arith.cpp:[[@LINE+1]]:14: error: unsupported: signed pointer offset (C++ allows a negative offset into the middle of an array; a Rust suffix slice cannot go backwards)
    return p[k];
}

int prev(const int* p) {
    // CHECK-NEXT: pointer-arith.cpp:[[@LINE+1]]:14: error: unsupported: pointer arithmetic `p - k` (a Rust suffix slice cannot go backwards)
    return *(p - 1);
}

void retreat(int* p) {
    // CHECK-NEXT: pointer-arith.cpp:[[@LINE+1]]:5: error: unsupported: pointer decrement (a Rust suffix slice cannot go backwards)
    --p;
    // CHECK-NEXT: pointer-arith.cpp:[[@LINE+1]]:5: error: unsupported: pointer decrement (a Rust suffix slice cannot go backwards)
    p--;
    // CHECK-NEXT: pointer-arith.cpp:[[@LINE+1]]:5: error: unsupported: pointer decrement (a Rust suffix slice cannot go backwards)
    p -= 2;
    *p = 0;
}

int offset(const int* p, long k) {
    // CHECK-NEXT: pointer-arith.cpp:[[@LINE+1]]:24: error: unsupported: signed pointer offset (C++ allows a negative offset into the middle of an array; a Rust suffix slice cannot go backwards)
    const int* q = p + k;
    return *q;
}

int cmp(const int* a, const int* b) {
    int r = 0;
    // CHECK-NEXT: pointer-arith.cpp:[[@LINE+1]]:9: error: unsupported: pointer comparison (Rust compares slices by contents, and suffixes of distinct objects can have equal lengths)
    if (a == b) { r += 1; }
    // CHECK-NEXT: pointer-arith.cpp:[[@LINE+1]]:9: error: unsupported: pointer comparison (Rust compares slices by contents, and suffixes of distinct objects can have equal lengths)
    if (a != b) { r += 2; }
    // CHECK-NEXT: pointer-arith.cpp:[[@LINE+1]]:9: error: unsupported: pointer comparison (Rust compares slices by contents, and suffixes of distinct objects can have equal lengths)
    if (a < b) { r += 4; }
    // CHECK-NEXT: pointer-arith.cpp:[[@LINE+1]]:9: error: unsupported: pointer comparison (Rust compares slices by contents, and suffixes of distinct objects can have equal lengths)
    if (a >= b) { r += 8; }
    // CHECK-NEXT: pointer-arith.cpp:[[@LINE+1]]:14: error: unsupported: pointer difference (a Rust slice has no address to subtract)
    long d = b - a;
    return r + static_cast<int>(d);
}

int swapped(const int* p) {
    // CHECK-NEXT: pointer-arith.cpp:[[@LINE+1]]:14: error: unsupported: pointer arithmetic other than `p + k`
    return *(2 + p);
}

int main() {
    // CHECK-NEXT: pointer-arith.cpp:[[@LINE+1]]:5: error: unsupported: variable of type 'int[4]'
    int v[4] = {1, 2, 3, 4};
    // CHECK-NEXT: pointer-arith.cpp:[[@LINE+1]]:29: error: unsupported: array subscript
    std::printf("%d\n", at(&v[0], 1));
    return 0;
}
