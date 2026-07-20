// W3.3 multi-TU gate oracle (G6 — FLIPPED to cell-slice): planCellSlices
// (CTS-P10) used to require the OWNING FUNCTION of every cell-slice
// parameter to be internal linkage unless this TU was the whole program
// (ImportCPlanning.cpp). Isolated from G5: `A` (the global base) is
// INTERNAL here; only `sum4` (the owning function) is externally visible,
// called from the companion TU too — on the companion's OWN internal
// global `B`. Real correctness-visible rejection: without a cell-slice
// class, `sum4(A)` hit the pre-CTS-P10 "passing a pointer into a global
// variable to a function" rejection.
//
// W3.3 G6 lifts it with the W3.2 whole-program cell-slice merge: `sum4` is
// only ever passed qualifying globals project-wide (A here, B in the
// companion — both INTERNAL, so they never merge into a multi-base class),
// so the generic `&[Cell<i32>]` parameter backs a DIFFERENT internal
// global per TU. The local-argument counterexample stays rejected: see
// multi-tu-gate-g6-cellslice-fn-negative.c.
//
// RUN: emitrust-import-c %s %S/Inputs/multi-tu-gate-g6-cellslice-fn-external-other.c | FileCheck %s

static int A[4];

int sum4(int *a) {
  int s = 0;
  int i;
  for (i = 0; i < 4; i++)
    s = s + a[i];
  return s;
}

int main(void) {
  int i;
  for (i = 0; i < 4; i++)
    A[i] = i + 1;
  return sum4(A);
}

// One generic cell-slice `sum4`, driven from two TUs with two DIFFERENT
// per-TU-tagged internal globals via distinct global_cells regions.
// CHECK: func.func @sum4(%{{.*}}: !emitrust.ref<!emitrust.cell_slice<i32>>) -> i32
// CHECK: emitrust.cell_get %{{.*}} : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64) -> i32
// CHECK: emitrust.global_cells @tu0_A {
// CHECK: func.call @sum4(%{{.*}}) : (!emitrust.ref<!emitrust.cell_slice<i32>>) -> i32
// CHECK: emitrust.global_cells @tu1_B {
// CHECK: func.call @sum4(%{{.*}}) : (!emitrust.ref<!emitrust.cell_slice<i32>>) -> i32
