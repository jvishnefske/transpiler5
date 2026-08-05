// RUN: emitrust-import-c %s | FileCheck %s

int sum_squares(void) {
  int a[4];
  for (int i = 0; i < 4; ++i) {
    a[i] = i * i;
  }
  int s = 0;
  for (int i = 0; i < 4; ++i) {
    s = s + a[i];
  }
  return s;
}

// The array local is an EmitRust place. FR-61f: both canonical counting
// loops (`for (int i = 0; i < 4; ++i)`) are recognized on the clang AST and
// emitted as `emitrust.for` range heads directly at import — no cf CFG, no
// promotable counter cell. The induction is the region's block argument,
// used DIRECTLY (body-immutable per matcher clause 4): no place, no seed
// store. The accumulator `s`, touched inside a lifted body, is routed to an
// `emitrust.variable` place (a `memref.alloca` cell could not promote across
// the region op).
// CHECK-LABEL: func.func @sum_squares
// CHECK: %[[A:.*]] = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.array<4xi32>>

// First loop: indexed write. The half-open bound `4` and unit step are the
// range operands; the i32 induction block argument feeds the subscript and
// the `i * i` multiply directly.
// CHECK: emitrust.for %[[IV:.*]] = %{{.*}} to %{{.*}} step %{{.*}} : i32 {
// CHECK: %[[ELT:.*]] = emitrust.subscript %[[A]][%[[IV]]] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
// CHECK: arith.muli %[[IV]], %[[IV]]
// CHECK: emitrust.assign %[[ELT]] = %{{.*}} : !emitrust.lvalue<i32>

// The accumulator is a place; its constant initializer rides the variable's
// init attribute (`<0 : i32>` -> `let mut s: i32 = 0;`, not a late assign).
// The second loop reads `a[i]` and accumulates.
// CHECK: %[[S:.*]] = emitrust.variable named "s" <0 : i32> : !emitrust.lvalue<i32>
// CHECK: emitrust.for %[[IV2:.*]] = %{{.*}} to %{{.*}} step %{{.*}} : i32 {
// CHECK: %[[ELT2:.*]] = emitrust.subscript %[[A]][%[[IV2]]] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
// CHECK: emitrust.load %[[ELT2]] : (!emitrust.lvalue<i32>) -> i32
// CHECK: arith.addi
// CHECK: return
