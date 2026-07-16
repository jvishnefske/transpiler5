// RUN: emitrust-import-c %s | FileCheck %s

// FR: Phase-1a pointer decomposition. Local pointers never materialize as
// MLIR pointer values: each one resolves to a single base object plus an
// i64 cursor in a promotable rank-0 memref cell. Dereference and subscript
// become emitrust.subscript(base, cursor); the address of a scalar is a
// degenerate (cursor-less) pointer that resolves to the scalar's own place.

struct S {
  int x;
  int y;
};

// A pointer to a scalar is degenerate: no cursor cell exists, `*p` and
// `p[0]` read and write the variable's own place, and no reference is
// ever materialized.
int deref_scalar(void) {
  int x = 41;
  int *p = &x;
  *p = 5;
  return *p + p[0];
}
// CHECK-LABEL: func.func @deref_scalar
// CHECK-NOT: memref.alloca() : memref<i64>
// CHECK: %[[X:.*]] = emitrust.variable : !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[X]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[X]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK: %[[A:.*]] = emitrust.load %[[X]] : (!emitrust.lvalue<i32>) -> i32
// CHECK: %[[B:.*]] = emitrust.load %[[X]] : (!emitrust.lvalue<i32>) -> i32
// CHECK: arith.addi %[[A]], %[[B]]
// CHECK-NOT: emitrust.addr_of
// CHECK: return

// `q = &arr[2]` stores the index into q's i64 cursor cell; `*q` subscripts
// the array at the loaded cursor.
int take_third(void) {
  int arr[4];
  arr[2] = 7;
  int *q = &arr[2];
  return *q;
}
// CHECK-LABEL: func.func @take_third
// CHECK: %[[QCELL:.*]] = memref.alloca() : memref<i64>
// CHECK: %[[ARR:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
// CHECK: memref.store %{{.*}}, %[[QCELL]][] : memref<i64>
// CHECK: %[[CUR:.*]] = memref.load %[[QCELL]][] : memref<i64>
// CHECK: %[[ELEM:.*]] = emitrust.subscript %[[ARR]][%[[CUR]]] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>
// CHECK: emitrust.load %[[ELEM]] : (!emitrust.lvalue<i32>) -> i32

// Array decay binds the cursor to 0; ++, +=, and -- walk the cursor.
int decay_walk(void) {
  int arr[4];
  arr[1] = 10;
  arr[3] = 30;
  arr[2] = 20;
  int *q = arr;
  q++;
  int a = *q;
  q += 2;
  int b = *q;
  --q;
  int c = *q;
  return a + b + c;
}
// CHECK-LABEL: func.func @decay_walk
// CHECK: %[[Q:.*]] = memref.alloca() : memref<i64>
// CHECK: %[[ARR:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
//   q = arr
// CHECK: %[[ZERO:.*]] = arith.constant 0 : i64
// CHECK: memref.store %[[ZERO]], %[[Q]][] : memref<i64>
//   q++
// CHECK: %[[C0:.*]] = memref.load %[[Q]][] : memref<i64>
// CHECK: %[[N1:.*]] = arith.addi %[[C0]], %{{.*}} : i64
// CHECK: memref.store %[[N1]], %[[Q]][] : memref<i64>
//   a = *q
// CHECK: %[[C1:.*]] = memref.load %[[Q]][] : memref<i64>
// CHECK: emitrust.subscript %[[ARR]][%[[C1]]]
//   q += 2 (the i32 amount widens to the i64 cursor)
// CHECK: %[[C2:.*]] = memref.load %[[Q]][] : memref<i64>
// CHECK: arith.extsi %{{.*}} : i32 to i64
// CHECK: %[[N2:.*]] = arith.addi %[[C2]], %{{.*}} : i64
// CHECK: memref.store %[[N2]], %[[Q]][] : memref<i64>
//   b = *q
// CHECK: %[[CB:.*]] = memref.load %[[Q]][] : memref<i64>
// CHECK: emitrust.subscript %[[ARR]][%[[CB]]]
//   --q
// CHECK: %[[C3:.*]] = memref.load %[[Q]][] : memref<i64>
// CHECK: %[[N3:.*]] = arith.subi %[[C3]], %{{.*}} : i64
// CHECK: memref.store %[[N3]], %[[Q]][] : memref<i64>

// A variable subscript on a walking pointer adds the (widened) index to
// the cursor.
int var_index(void) {
  int arr[4];
  arr[3] = 9;
  int k = 2;
  int *q = &arr[1];
  return q[k];
}
// CHECK-LABEL: func.func @var_index
// CHECK: %[[CUR:.*]] = memref.load %{{.*}}[] : memref<i64>
// CHECK: %[[K:.*]] = memref.load %{{.*}}[] : memref<i32>
// CHECK: %[[KW:.*]] = arith.extsi %[[K]] : i32 to i64
// CHECK: %[[IDX:.*]] = arith.addi %[[CUR]], %[[KW]] : i64
// CHECK: emitrust.subscript %{{.*}}[%[[IDX]]] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>

// `*(q++)` in value position subscripts at the pre-increment cursor.
int post_inc_value(void) {
  int arr[2];
  arr[0] = 4;
  arr[1] = 6;
  int *q = arr;
  int first = *(q++);
  return first + *q;
}
// CHECK-LABEL: func.func @post_inc_value
// CHECK: %[[PRE:.*]] = memref.load %{{.*}}[] : memref<i64>
// CHECK: %[[NEXT:.*]] = arith.addi %[[PRE]], %{{.*}} : i64
// CHECK: memref.store %[[NEXT]], %{{.*}}[] : memref<i64>
// CHECK: emitrust.subscript %{{.*}}[%[[PRE]]]

// Same-object pointer difference is the plain i64 cursor difference
// (ptrdiff_t is long); same-object comparison is a signed i64 cmpi.
long diff_and_compare(void) {
  int arr[4];
  int *q = &arr[0];
  int *r = &arr[3];
  long d = r - q;
  if (q < r) {
    d = d + (&arr[2] - &arr[1]);
  }
  return d;
}
// CHECK-LABEL: func.func @diff_and_compare
// CHECK: arith.subi %{{.*}}, %{{.*}} : i64
// CHECK: arith.cmpi slt, %{{.*}}, %{{.*}} : i64
// CHECK: arith.subi %{{.*}}, %{{.*}} : i64
// CHECK: arith.addi %{{.*}}, %{{.*}} : i64

// `->` through a degenerate struct pointer resolves to the struct's own
// place; through a walking pointer into a struct array it resolves to the
// subscripted element (the 00018 shape).
int arrow(void) {
  struct S s;
  struct S *p = &s;
  p->x = 1;
  p->y = 2;
  struct S t[3];
  struct S *w = t;
  w->x = 5;
  w++;
  w->y = 6;
  return p->x + t[1].y;
}
// CHECK-LABEL: func.func @arrow
// CHECK: %[[W:.*]] = memref.alloca() : memref<i64>
// CHECK: %[[SVAR:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"S">>
// CHECK: emitrust.member %[[SVAR]]["x"]
// CHECK: emitrust.assign
// CHECK: emitrust.member %[[SVAR]]["y"]
// CHECK: emitrust.assign
// CHECK: %[[T:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<3x!emitrust.struct<"S">>>
// CHECK: memref.store %{{.*}}, %[[W]][] : memref<i64>
// CHECK: %[[WC:.*]] = memref.load %[[W]][] : memref<i64>
// CHECK: %[[ELEM:.*]] = emitrust.subscript %[[T]][%[[WC]]] : (!emitrust.lvalue<!emitrust.array<3x!emitrust.struct<"S">>>, i64) -> !emitrust.lvalue<!emitrust.struct<"S">>
// CHECK: emitrust.member %[[ELEM]]["x"]

// Rebinding a pointer to a different element of the SAME object stays in
// one region and simply stores a new cursor.
int rebind_same_base(void) {
  int arr[4];
  arr[0] = 1;
  arr[3] = 9;
  int *q = &arr[0];
  int a = *q;
  q = &arr[3];
  return a + *q;
}
// CHECK-LABEL: func.func @rebind_same_base
// CHECK: %[[Q:.*]] = memref.alloca() : memref<i64>
// CHECK: memref.store %{{.*}}, %[[Q]][] : memref<i64>
// CHECK: memref.load %[[Q]][] : memref<i64>
// CHECK: memref.store %{{.*}}, %[[Q]][] : memref<i64>
// CHECK: memref.load %[[Q]][] : memref<i64>
