// W3.3 multi-TU gate oracle (G4 — FLIPPED to cell-slice): planCellSlices
// (CTS-P10) used to poison an externally visible function's OWN pointer
// parameters preemptively (ImportCPlanning.cpp) — another TU could call it
// with a local argument this TU never sees, and a cell-slice parameter has
// no representation for a non-global (Cell-less) base. That poison was
// UNION-FIND-WIDE: `pub_fn` forwards its parameter to `helper`, so
// `pub_fn`'s preemptive poison cascaded to `helper` too. UNLIKE G3
// (owners), this was a genuine correctness-visible rejection, not an
// optimization loss: with no cell-slice class, `pub_fn(A)` hit the
// pre-CTS-P10 "passing a pointer into a global variable to a function"
// rejection.
//
// W3.3 G4 lifts the poison with the W3.2 whole-program cell-slice merge:
// `pub_fn` is referenced project-wide only with the qualifying global `A`
// (the companion's `unrelated_g4` never calls it), so its parameter — and,
// through the forwarding class, `helper`'s — become cell-slices. The
// multi-base/local counterexamples stay rejected: see
// multi-tu-gate-g4-cellslice-local-arg.c.
//
// RUN: emitrust-import-c %s %S/Inputs/multi-tu-gate-g4-cellslice-poison-other.c | FileCheck %s

int A[4];

static int helper(int *a) { return a[0]; }

int pub_fn(int *a) { return helper(a); }

int main(void) {
  int i;
  for (i = 0; i < 4; i++)
    A[i] = i + 1;
  return pub_fn(A);
}

// The internal helper (per-TU tag) and the external pub_fn both take a
// generic cell-slice; helper reads it with cell_get, pub_fn forwards the
// same reference on.
// CHECK-LABEL: func.func @tu0_helper
// CHECK-SAME: (%{{.*}}: !emitrust.ref<!emitrust.cell_slice<i32>>) -> i32
// CHECK: emitrust.cell_get %{{.*}}[%{{.*}}] : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64) -> i32
// CHECK-LABEL: func.func @pub_fn
// CHECK-SAME: (%[[P:.*]]: !emitrust.ref<!emitrust.cell_slice<i32>>) -> i32
// CHECK: call @tu0_helper(%[[P]]) : (!emitrust.ref<!emitrust.cell_slice<i32>>) -> i32
// The call site binds the concrete global `A` via a global_cells region.
// CHECK-LABEL: func.func @c_main
// CHECK: emitrust.global_cells @A {
// CHECK: func.call @pub_fn(%{{.*}}) : (!emitrust.ref<!emitrust.cell_slice<i32>>) -> i32
