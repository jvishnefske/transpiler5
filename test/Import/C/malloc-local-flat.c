// RUN: emitrust-import-c %s | FileCheck %s

// W4.2e Part A: a constant-size heap allocation bound to a LOCAL pointer
// decomposes against a synthesized MUTABLE backing array plus an i64
// cursor cell — the writable, function-scope analog of a string-literal
// region. `malloc(cap * sizeof(int))` folds its size through the
// foldable-local resolver (`cap` is an automatic local, never reassigned
// nor address-taken, so it folds to its initializer), yielding a
// `[CAP x i32]` backing. Subscripts read and write the backing directly;
// `free` of the pointer is a no-op (the backing drops at scope end).

#include <stdlib.h>

int flat(void) {
  int cap = 4;
  int *p = malloc(cap * sizeof(int));
  p[0] = 7;
  int r = p[0];
  free(p);
  return r;
}
// CHECK-LABEL: func.func @flat
// The cursor cell (i64) and the mutable backing array are synthesized at
// entry; the cursor initializes to 0 (the base of a fresh allocation).
// CHECK: %[[CUR:.*]] = memref.alloca() : memref<i64>
// CHECK: %[[BACK:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
// CHECK: memref.store %{{.*}}, %[[CUR]][] : memref<i64>
// A write through the pointer subscripts the mutable backing and assigns.
// CHECK: %[[E0:.*]] = emitrust.subscript %[[BACK]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[E0]] = %{{.*}} : !emitrust.lvalue<i32>
// A read subscripts the same backing and loads.
// CHECK: %[[E1:.*]] = emitrust.subscript %[[BACK]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>
// CHECK: emitrust.load %[[E1]] : (!emitrust.lvalue<i32>) -> i32
// `free(p)` emits no deallocation — no call to a `free` symbol survives.
// CHECK-NOT: func.call @free
// CHECK: return

// A `calloc`-bound local decomposes the same way, and the constant count
// times element size folds to the backing extent.
int calloc_flat(void) {
  int *p = calloc(3, sizeof(int));
  p[2] = 5;
  return p[2];
}
// CHECK-LABEL: func.func @calloc_flat
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.array<3xi32>>
