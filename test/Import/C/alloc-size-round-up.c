// RUN: emitrust-import-c %s | FileCheck %s

// FR-230, the SYMMETRY pin for the allocation-size rounding.
//
// A constant byte size that does not divide evenly by the pointer's element
// size used to be a flat rejection ("allocation size does not fit the
// pointer's element type"). It now rounds UP to whole elements, because the
// only C program that can observe the difference -- `int *p = alloca(10)`
// followed by `p[0..9]` -- is writing 40 bytes into a 10-byte object, which
// is UNDEFINED, and a `[i32; 3]` backing turns that into a Rust `index out
// of bounds` panic instead of a silent stack scribble. That is the loud
// direction, the same class as FR-229's over-length byte-view read.
//
// THIS FILE EXISTS TO KEEP THE ROUNDING FROM WIDENING. `ceil` and exact
// division agree on every size that already divided evenly, so no
// previously admitted allocation may change its backing extent by so much
// as one element. Every EXACT case below is pinned to the extent it had
// before FR-230; if a later refactor generalizes the rounding (scaling to a
// different element unit, rounding to a power of two, padding "for safety"),
// one of these extents moves and this file fails. The rounding is scoped to
// exactly one thing: a compile-time-constant total with a remainder.

#include <stdlib.h>

// EXACT, and pinned: 16 bytes over a 4-byte element is 4 elements, not 5.
int malloc_exact(void) {
  int *p = (int *)malloc(16);
  p[3] = 1;
  return p[3];
}
// CHECK-LABEL: func.func @malloc_exact
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>

// EXACT at a byte element: 10 bytes over a 1-byte element is 10, and the
// rounding must not touch it. This is the counterpart of the rounded case
// below at the SAME byte size -- the difference is the element type alone.
int malloc_exact_bytes(void) {
  char *p = (char *)malloc(10);
  p[9] = 'x';
  return p[9];
}
// CHECK-LABEL: func.func @malloc_exact_bytes
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.array<10xi8>>

// EXACT through the calloc pair: 3 * 8 == 24 bytes over an 8-byte element
// is 3 elements.
double calloc_exact(void) {
  double *p = (double *)calloc(3, sizeof(double));
  p[2] = 1.0;
  return p[2];
}
// CHECK-LABEL: func.func @calloc_exact
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.array<3xf64>>

// ROUNDED: 10 bytes over a 4-byte element is 3 elements (ceil(10/4)), not 2
// and not 4. Two whole ints fit and the third is the partial one the C
// access would run into; the backing covers it so the out-of-range element
// is a bounds panic and not a scribble.
int alloc_rounded(void) {
  int *p = (int *)malloc(10);
  p[0] = 1;
  return p[0];
}
// CHECK-LABEL: func.func @alloc_rounded
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.array<3xi32>>

// ROUNDED by exactly one byte: 9 bytes over an 8-byte element is 2, which
// is the smallest rounding that can happen and the one a "round to the
// nearest" bug would get wrong.
double round_by_one(void) {
  double *p = (double *)malloc(9);
  p[0] = 2.0;
  return p[0];
}
// CHECK-LABEL: func.func @round_by_one
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.array<2xf64>>

// ROUNDED through calloc's two-argument form: 3 * 5 == 15 bytes over a
// 4-byte element is 4.
int calloc_rounded(void) {
  int *p = (int *)calloc(3, 5);
  p[0] = 1;
  return p[0];
}
// CHECK-LABEL: func.func @calloc_rounded
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
