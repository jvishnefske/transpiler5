// FR-261: the pointer uses the suffix-slice model does not cover (R6) are
// LOCATED refusals, each in the existing wording where one existed (the
// census keys on it). Pins: null -- `nullptr` / `0` as a pointer, `if (p)`,
// `!p`, `q != nullptr` (the subset's pointers always point into an object;
// Option is not in the subset); a pointer data member and a returned pointer
// (no lifetime to give either); `T**`, `void*`, function pointers; pointer
// casts (`const_cast`, `reinterpret_cast`, a C-style or `static_cast` to a
// pointer type); libc buffer calls (`memcpy`, `memset`); whole-pointer
// assignment `p = q;` (a parameter's Rust lifetime is the caller's -- only
// the forward re-slice `p += k` is in the subset); the address of a pointer;
// indexing or member access through a pointer EXPRESSION rather than a
// pointer variable; and `std::string::data` / `c_str` outside printf.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}24{{$}}
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// CHECK: pointer-other.cpp:[[@LINE+1]]:1: error: unsupported: struct with members outside the subset
struct Node {
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:10: error: unsupported: data member of type 'int *'
    int* next;
    int val;
};

struct Pair {
    int a;
    int b;
};

// CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:12: error: unsupported: return type 'const int *'
const int* first(const int* p) {
    return p;
}

// CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:15: error: unsupported: parameter of type 'int **'
void pp(int** q) {
}

// CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:15: error: unsupported: parameter of type 'void *'
void vp(void* q) {
}

// CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:15: error: unsupported: parameter of type 'int (*)(int)'
void fp(int (*g)(int)) {
}

int nulls(int* p, const int* q) {
    int r = 0;
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:9: error: unsupported: null pointer (the subset's pointers always point into an object)
    if (p) { r += 1; }
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:9: error: unsupported: null pointer (the subset's pointers always point into an object)
    if (q != nullptr) { r += 2; }
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:10: error: unsupported: null pointer (the subset's pointers always point into an object)
    if (!p) { r += 4; }
    return r;
}

void casts(const int* p, Pair* s) {
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:14: error: unsupported: const_cast
    int* m = const_cast<int*>(p);
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:30: error: unsupported: reinterpret_cast
    const unsigned char* b = reinterpret_cast<const unsigned char*>(p);
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:20: error: unsupported: cast to 'const int *'
    const int* c = (const int*)s;
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:21: error: unsupported: cast to 'const long *'
    const long* d = static_cast<const long*>(static_cast<const void*>(p));
}

void libc(int* d, const int* s) {
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:5: error: unsupported: call to library function 'memcpy'
    std::memcpy(d, s, 4);
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:5: error: unsupported: call to library function 'memset'
    std::memset(d, 0, 4);
}

void shapes(const int* p, const int* q, Pair* s, std::vector<int>& v, std::string& text) {
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:5: error: unsupported: assignment of a pointer (the subset re-slices a pointer only by `p += k;` / `++p;`)
    p = q;
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:5: error: unsupported: variable of type 'const int *const *'
    const int* const* pp2 = &p;
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:13: error: unsupported: array subscript
    int x = (p + 1)[0];
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:13: error: unsupported: member access through a pointer
    int y = (s + 1)->a;
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:21: error: unsupported: std::string::data
    const char* c = text.data();
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:21: error: unsupported: std::string::c_str outside a printf %s argument
    const char* t = text.c_str();
    std::printf("%d %d\n", x, y);
}

int main() {
    int k = 1;
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:35: error: unsupported: null pointer (the subset's pointers always point into an object)
    std::printf("%d\n", nulls(&k, nullptr));
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:11: error: unsupported: null pointer (the subset's pointers always point into an object)
    nulls(nullptr, &k);
    // CHECK-NEXT: pointer-other.cpp:[[@LINE+1]]:15: error: unsupported: null pointer (the subset's pointers always point into an object)
    int* np = 0;
    return 0;
}
