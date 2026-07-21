// W3.4 multi-TU gate oracle (G8 — FLIPPED to accept): importPointerGlobal
// (CTS-P4, ImportCGlobals.cpp) used to hard-reject any REFERENCED externally
// visible pointer-typed global in a multi-file project — planOwners'
// pointer-region facts (`globalPtrFacts`) are merged per TU, so another TU
// could rebind it behind this TU's already-consumed facts.
//
// This test isolates the gate that fires when THIS TU holds the real
// definition and references it from within its own `read_g`/`main` (the
// companion is unrelated). W3.4 G8 relaxes it with the W3.2 COMMIT B
// whole-program pointer-global facts: `g` is bound project-wide to the ONE
// file-scope base `arr` with NO reassignment anywhere (the same eligibility
// `deferExternPointerGlobal` uses for the extern-declaration side), so it is
// reconstructible as the CTS-P4 single-base cursor global. The divergent
// cross-TU rebinding stays rejected: see
// multi-tu-gate-g8-ptr-global-negative.c.
//
// RUN: emitrust-import-c %s %S/Inputs/multi-tu-gate-g8-ptr-global-external-other.c | FileCheck %s

int arr[4];
int *g = &arr[0];
int read_g(void) { return *g; }
int main(void) { return read_g(); }

// `g` becomes the CTS-P4 single-base cursor global (an i64 offset against
// the base object `arr`); `*g` stages `arr` and subscripts it at the cursor.
// CHECK-DAG: emitrust.global @arr : !emitrust.array<4xi32>
// CHECK-DAG: emitrust.global @g <0 : i64> : i64
// CHECK-LABEL: func.func @read_g
// CHECK: emitrust.global_load @g : i64
// CHECK: emitrust.global_load @arr : !emitrust.array<4xi32>
// CHECK: emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>
