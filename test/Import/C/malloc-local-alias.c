// RUN: emitrust-import-c %s | FileCheck %s

// FR-146: ONE heap-allocation region has ONE backing array. The pointer
// region analysis is union-find over a pointer's sources, so `char *q = p;`
// UNITES q with p's region — both pointers name the same storage, and both
// must decompose against the same synthesized `emitrust.variable` backing,
// carrying only their own independent i64 cursor cells.
//
// The importer used to create the backing per VARIABLE, which gave the
// second pointer a private zeroed array: a write through one pointer was
// invisible through the other. That is a silent miscompile — the emitted
// crate compiled and ran and printed the wrong bytes — and it reached
// emission unnoticed because the shapes that expose it (a string builtin
// on the aliasing pointer, or passing it to a function) crashed the
// importer first. It is pinned here on the MLIR and, byte for byte
// against the native binary, in EndToEnd/malloc-region-string-fn.c.

#include <stdlib.h>

// Two pointer locals, one allocation: ONE backing, TWO cursor cells.
int alias(void) {
  char *p = (char *)malloc(8);
  char *q = p;
  q[1] = 5;
  return p[1];
}
// CHECK-LABEL: func.func @alias
// CHECK: %[[CQ:.*]] = memref.alloca() : memref<i64>
// CHECK: %[[CP:.*]] = memref.alloca() : memref<i64>
// CHECK: %[[BACK:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>
// Exactly one backing array is synthesized for the region.
// CHECK-NOT: emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>
// The write through `q` and the read through `p` subscript the SAME place.
// CHECK: %[[WE:.*]] = emitrust.subscript %[[BACK]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<8xi8>>, i64) -> !emitrust.lvalue<i8>
// CHECK: emitrust.assign %[[WE]] = %{{.*}} : !emitrust.lvalue<i8>
// CHECK: %[[RE:.*]] = emitrust.subscript %[[BACK]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<8xi8>>, i64) -> !emitrust.lvalue<i8>
// CHECK: emitrust.load %[[RE]] : (!emitrust.lvalue<i8>) -> i8
// CHECK: return

// An OFFSET alias shares the backing too; only the cursors differ, so the
// aliasing write lands at the offset element of the one region.
int alias_offset(void) {
  int *p = (int *)malloc(4 * sizeof(int));
  int *q = p + 2;
  q[0] = 9;
  return p[2];
}
// CHECK-LABEL: func.func @alias_offset
// CHECK: %[[OBACK:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
// CHECK-NOT: emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
// CHECK: emitrust.subscript %[[OBACK]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>
// CHECK: emitrust.assign
// CHECK: emitrust.subscript %[[OBACK]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>
// CHECK: return

// CONTROL: two DISTINCT allocations keep two distinct backings — the
// sharing is keyed on the region's alloc site, not on the pointee type.
int distinct(void) {
  char *a = (char *)malloc(8);
  char *b = (char *)malloc(8);
  a[0] = 1;
  b[0] = 2;
  return a[0] + b[0];
}
// CHECK-LABEL: func.func @distinct
// CHECK: %[[A:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>
// CHECK: %[[B:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>
// CHECK: emitrust.subscript %[[A]][%{{.*}}]
// CHECK: emitrust.assign
// CHECK: emitrust.subscript %[[B]][%{{.*}}]
// CHECK: emitrust.assign
// CHECK: return
