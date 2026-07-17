// RUN: emitrust-import-c %s | FileCheck %s

// CTS-P2: a qualification-preserving explicit pointer cast — a C-style
// cast between data-pointer types with the same unqualified pointee —
// changes nothing the (base, cursor) decomposition tracks, so it peels
// transparently in both the region analysis and the emission. Casts that
// genuinely reinterpret the pointee stay rejected (see
// pointers-member-invalid.c, reinterpret-cast.c).

int cast_peel(void) {
  int arr[2];
  int *p = (int *)arr;
  p[0] = 5;
  int *q;
  q = (int *)(p + 1);
  return q[-1];
}
// CHECK-LABEL: func.func @cast_peel
//   `(int *)arr` binds the array base; p[0] subscripts it at the cursor.
// CHECK: %[[ARR:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<2xi32>>
// CHECK: emitrust.subscript %[[ARR]][{{.*}}] : (!emitrust.lvalue<!emitrust.array<2xi32>>, i64) -> !emitrust.lvalue<i32>
//   `(int *)(p + 1)` joins q into the same region with cursor arithmetic.
// CHECK: arith.addi
// CHECK: emitrust.subscript %[[ARR]][{{.*}}] : (!emitrust.lvalue<!emitrust.array<2xi32>>, i64) -> !emitrust.lvalue<i32>

// A const-qualification adjustment peels the same way.
int qual_peel(void) {
  int x = 9;
  const int *cp = (const int *)&x;
  return *cp;
}
// CHECK-LABEL: func.func @qual_peel
// CHECK: %[[X:.*]] = emitrust.variable : !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[X]]
// CHECK: emitrust.load %[[X]]
