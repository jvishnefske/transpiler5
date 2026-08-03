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

// The array local is an EmitRust place; the scalar loop counters stay
// promotable memref cells.
// CHECK-LABEL: func.func @sum_squares
// CHECK: memref.alloca() : memref<i32>
// CHECK: %[[A:.*]] = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.array<4xi32>>
// CHECK: arith.cmpi slt
// CHECK: cf.cond_br

// Indexed write: the i32 index SSA value is passed directly.
// CHECK: %[[ELT:.*]] = emitrust.subscript %[[A]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
// CHECK: arith.muli
// CHECK: emitrust.assign %[[ELT]] = %{{.*}} : !emitrust.lvalue<i32>

// Indexed read in the second loop.
// CHECK: %[[ELT2:.*]] = emitrust.subscript %[[A]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
// CHECK: emitrust.load %[[ELT2]] : (!emitrust.lvalue<i32>) -> i32
// CHECK: arith.addi
// CHECK: return
