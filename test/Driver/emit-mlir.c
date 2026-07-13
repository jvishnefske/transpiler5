// FR-20: the pipeline-stage debugging outputs. --emit=import prints the raw
// imported module (core dialects, before any pass); --emit=mlir prints the
// module after the full pass pipeline (pure EmitRust, the emitter's input).
// RUN: emitrust-cc --emit=import %s -o - | FileCheck %s --check-prefix=IMPORT
// RUN: emitrust-cc --emit=mlir %s -o - | FileCheck %s

int add(int a, int b) { return a + b; }

int main(void) { return add(1, 2); }

// Before any pass: core func functions with the c_main rename, scalar
// locals still visible as core-dialect operations.
// IMPORT: func.func @add
// IMPORT: func.func @c_main

// After the pipeline: only EmitRust functions remain.
// CHECK-NOT: func.func
// CHECK: emitrust.func @add
// CHECK-NOT: func.func
// CHECK: emitrust.func @c_main
// CHECK-NOT: func.func
