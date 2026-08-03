// RUN: emitrust-import-c %s | FileCheck %s

// FR: CTS-P5 second-order pointers. Under the Phase-1a decomposition a
// first-order pointer is a plain Copy i64 cursor into its region, so a
// pointer-to-pointer is a cursor into a region of cursor cells: an index
// selecting WHICH first-order pointer to operate on. The implemented
// shape is the degenerate one-cell region — `pp` is only ever bound to
// the address of a single first-order pointer local — so the selection is
// static, `pp` needs no runtime state, `*pp` reads or rebinds the
// selected pointer's (base, cursor) decomposition, and `**pp`
// dereferences it. No reference-to-reference ever arises: the cursor-cell
// region and the pointee region stay distinct.

// The 00005/00020 shape: `p` is degenerate (scalar base, no cursor), so
// `**pp` reads and writes x's own place; neither `p` nor `pp` has any
// runtime state, and no reference is ever materialized.
int deref_deref_scalar(void) {
  int x;
  int *p;
  int **pp;
  x = 0;
  p = &x;
  pp = &p;
  **pp = 41;
  return **pp + x;
}
// CHECK-LABEL: func.func @deref_deref_scalar
// CHECK-NOT: memref.alloca
// CHECK: %[[X:.*]] = emitrust.variable named "x" : !emitrust.lvalue<i32>
// CHECK-NOT: emitrust.addr_of
// CHECK: emitrust.assign %[[X]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK: %[[A:.*]] = emitrust.load %[[X]] : (!emitrust.lvalue<i32>) -> i32
// CHECK: %[[B:.*]] = emitrust.load %[[X]] : (!emitrust.lvalue<i32>) -> i32
// CHECK: arith.addi %[[A]], %[[B]]
// CHECK: return

// A cursor-carrying target: `**pp` subscripts the array at p's current
// cursor, `*pp = &arr[2]` re-points p by storing into p's own cursor
// cell, and a repeated `pp = &p` of the same target emits nothing.
int cursor_repoint(void) {
  int arr[4];
  arr[1] = 10;
  arr[2] = 20;
  int *p = &arr[1];
  int **pp = &p;
  int a = **pp;
  *pp = &arr[2];
  pp = &p;
  **pp = 30;
  return a + arr[2];
}
// CHECK-LABEL: func.func @cursor_repoint
// CHECK: %[[PCELL:.*]] = memref.alloca() : memref<i64>
// CHECK: %[[ARR:.*]] = emitrust.variable named "arr" : !emitrust.lvalue<!emitrust.array<4xi32>>
// CHECK: memref.store %{{.*}}, %[[PCELL]][] : memref<i64>
// CHECK: %[[CUR1:.*]] = memref.load %[[PCELL]][] : memref<i64>
// CHECK: %[[E1:.*]] = emitrust.subscript %[[ARR]][%[[CUR1]]] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>
// CHECK: emitrust.load %[[E1]] : (!emitrust.lvalue<i32>) -> i32
// CHECK: memref.store %{{.*}}, %[[PCELL]][] : memref<i64>
// CHECK: %[[CUR2:.*]] = memref.load %[[PCELL]][] : memref<i64>
// CHECK: %[[E2:.*]] = emitrust.subscript %[[ARR]][%[[CUR2]]] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[E2]] = %{{.*}} : !emitrust.lvalue<i32>

// A nullable target keeps its Option-of-cursor discriminant through the
// second order: `*pp = &x` stores the target's non-null flag, `if (*pp)`
// reads it, and `**pp` derefs behind the deterministic null-check panic.
int nullable_target(void) {
  int x = 5;
  int *p = 0;
  int **pp = &p;
  *pp = &x;
  if (*pp)
    return **pp;
  return -1;
}
// CHECK-LABEL: func.func @nullable_target
// CHECK: %[[FLAG:.*]] = memref.alloca() : memref<i1>
// CHECK: %[[FALSE:.*]] = arith.constant false
// CHECK: memref.store %[[FALSE]], %[[FLAG]][] : memref<i1>
// CHECK: %[[TRUE:.*]] = arith.constant true
// CHECK: memref.store %[[TRUE]], %[[FLAG]][] : memref<i1>
// CHECK: %[[NN:.*]] = memref.load %[[FLAG]][] : memref<i1>
// CHECK: cf.cond_br %[[NN]]
// CHECK: emitrust.call_opaque "assert!"(%{{.*}}) {args = [0 : index, "null pointer dereference"]}
