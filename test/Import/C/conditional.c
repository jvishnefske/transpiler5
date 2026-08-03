// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | emitrust-opt --mem2reg --canonicalize --lift-cf-to-scf --canonicalize | FileCheck %s --check-prefix=SCF

int pick(int c, int a, int b) {
  return c ? a + 1 : b - 1;
}

// The conditional operator lowers to a cf diamond around a rank-0 result
// cell: each arm evaluates in its own block and stores, the merge block
// loads. Only the selected arm's side effects run.
// CHECK-LABEL: func.func @pick
// CHECK: %[[CELL:.*]] = memref.alloca() : memref<i32>
// CHECK: cf.cond_br %{{.*}}, ^[[TRUE:bb[0-9]+]], ^[[FALSE:bb[0-9]+]]
// CHECK: ^[[TRUE]]:
// CHECK: arith.addi
// CHECK: memref.store %{{.*}}, %[[CELL]][]
// CHECK: cf.br ^[[END:bb[0-9]+]]
// CHECK: ^[[FALSE]]:
// CHECK: arith.subi
// CHECK: memref.store %{{.*}}, %[[CELL]][]
// CHECK: cf.br ^[[END]]
// CHECK: ^[[END]]:
// CHECK: memref.load %[[CELL]][]

// After mem2reg and cf lifting the cell disappears into structured values.
// SCF-LABEL: func.func @pick
// SCF-NOT: memref.alloca

int effects(int c) {
  int x = 0;
  int y = 0;
  // Each arm's assignment is a side effect that must only run when its arm
  // is selected.
  int r = c ? (x = 10) : (y = 20);
  return r + x + y;
}

// CHECK-LABEL: func.func @effects
// CHECK: cf.cond_br

int nested(int c, int d) {
  return c ? (d ? 1 : 2) : 3;
}

// CHECK-LABEL: func.func @nested
// SCF-LABEL: func.func @nested

double mixed(int c, int i, double d) {
  // The int arm arrives behind clang's implicit int-to-double conversion,
  // so both arms map to f64.
  return c ? i : d;
}

// CHECK-LABEL: func.func @mixed
// CHECK: %[[DCELL:.*]] = memref.alloca() : memref<f64>
// CHECK: arith.sitofp %{{.*}} : i32 to f64
// CHECK: memref.store %{{.*}}, %[[DCELL]][]

int in_condition(int c, int a) {
  // A conditional operator used as a condition is truth-tested like any
  // other integer value.
  if (c ? a : 7) {
    return 1;
  }
  return 0;
}

// CHECK-LABEL: func.func @in_condition

unsigned int upick(int c, unsigned int a, unsigned int b) {
  // Unsigned results cannot live in memref cells (mem2reg would
  // materialize a signless default); the result flows through an
  // emitrust.variable place instead.
  return c ? a : b;
}

// CHECK-LABEL: func.func @upick
// The first two variables are the unsigned parameters' places; the third
// is the conditional's result cell.
// CHECK: emitrust.variable named "a" : !emitrust.lvalue<ui32>
// CHECK: emitrust.variable named "b" : !emitrust.lvalue<ui32>
// CHECK: %[[UCELL:.*]] = emitrust.variable : !emitrust.lvalue<ui32>
// CHECK: cf.cond_br
// CHECK: emitrust.assign %[[UCELL]] = %{{.*}} : !emitrust.lvalue<ui32>
// CHECK: emitrust.load %[[UCELL]] : (!emitrust.lvalue<ui32>) -> ui32
