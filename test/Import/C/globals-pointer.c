// RUN: emitrust-import-c %s | FileCheck %s

// CTS-P4: pointer-typed globals decompose against a global region base.
// A stored global cursor is a plain Copy i64 `emitrust.global` under the
// pointer's C name (no borrow is ever stored), and every element access
// stages the base global's whole value exactly like a direct global
// element access. The four supported base shapes are exercised below:
//  - `p`: a global scalar (degenerate; p needs no runtime state and no
//    module ops at all — every `*p` resolves statically to `x`),
//  - `q`: a global array (cursor global, initialized to the initializer's
//    element offset, rebound/walked at runtime),
//  - `s`: a file-scope compound literal (synthesized constant-initialized
//    `s_backing` global; degenerate),
//  - `t`: a single constant-size calloc site (synthesized zero-initialized
//    `t_backing` array global plus a cursor; the assignment re-zeroes the
//    backing and resets the cursor, exact calloc semantics).
// `unused_ptr` is never referenced and imports nothing (referenced-only
// policy), which also covers unused pointer declarations with incomplete
// pointee types (00209).

#include <stdlib.h>

int x = 5;
int *p = &x;
int arr[4];
int *q = arr + 1;
struct S { int a; int b; };
struct S *s = &(struct S){ 1, 2 };
int *t;
int *unused_ptr;

// CHECK: emitrust.global @x <5 : i32> : i32
// CHECK: emitrust.global @arr : !emitrust.array<4xi32>
// CHECK: emitrust.global @q <1 : i64> : i64
// CHECK: emitrust.global @s_backing <[1 : i32, 2 : i32]> : !emitrust.struct<"S">
// CHECK: emitrust.global @t_backing : !emitrust.array<8xi32>
// CHECK: emitrust.global @t <0 : i64> : i64
// CHECK-NOT: unused_ptr

// Degenerate global pointer: `*p` is a staged read of `x`, no cursor.
// CHECK-LABEL: func.func @readp
// CHECK: emitrust.global_load @x : i32
// CHECK-NOT: global_load @p
int readp(void) { return *p; }

// Compound-literal base: `s->a` refines the staged backing copy.
// CHECK-LABEL: func.func @reads
// CHECK: emitrust.global_load @s_backing : !emitrust.struct<"S">
// CHECK: emitrust.member {{.*}}["a"]
int reads(void) { return s->a; }

// Cursor walk: `q++` is load-add-store on the cursor global.
// CHECK-LABEL: func.func @step
// CHECK: emitrust.global_load @q : i64
// CHECK: arith.addi
// CHECK: emitrust.global_store {{.*}}, @q : i64
void step(void) { q++; }

// Data-dependent rebinding: `q = &arr[i]` stores the computed cursor.
// CHECK-LABEL: func.func @rebind
// CHECK: emitrust.global_store {{.*}}, @q : i64
void rebind(int i) { q = &arr[i]; }

// A write through the cursor stages the array, assigns the element, and
// stores the whole staged copy back (load-modify-store).
// CHECK-LABEL: func.func @writeq
// CHECK: emitrust.global_load @q : i64
// CHECK: emitrust.global_load @arr : !emitrust.array<4xi32>
// CHECK: emitrust.subscript
// CHECK: emitrust.global_store {{.*}}, @arr : !emitrust.array<4xi32>
void writeq(int v) { *q = v; }

// The calloc binding re-zeroes the backing via a fresh default-initialized
// staged value and resets the cursor; no call is ever emitted.
// CHECK-LABEL: func.func @seed
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi32>>
// CHECK: emitrust.global_store {{.*}}, @t_backing : !emitrust.array<8xi32>
// CHECK: emitrust.global_store {{.*}}, @t : i64
// CHECK-NOT: call
void seed(void) { t = calloc(8, sizeof(int)); }

// CHECK-LABEL: func.func @readt
// CHECK: emitrust.global_load @t : i64
// CHECK: emitrust.global_load @t_backing : !emitrust.array<8xi32>
int readt(int i) { return t[i]; }

int main(void) { return 0; }
