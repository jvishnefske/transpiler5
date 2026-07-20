// W3.3 multi-TU gate oracle (G5 — FLIPPED to cell-slice): planCellSlices
// (CTS-P10) used to require every global base of a cell-slice class to be
// internal-linkage unless this TU was the whole program
// (ImportCPlanning.cpp). `region` here is a realistic exported region-API
// shape (a global buffer plus an externally visible function operating on
// it, called from `main` in this TU AND from the companion TU on the SAME
// external `region`). Like G4 (and unlike G3's owners), this was a real
// correctness-visible rejection: `region_fill(region, 8)` hit the
// pre-CTS-P10 "passing a pointer into a global variable to a function"
// rejection.
//
// W3.3 G5 lifts it with the W3.2 whole-program cell-slice merge: the ONE
// external global `region` backs `region_fill`'s parameter across the
// whole project (a single external base — no multi-base divergence), so
// both become cell-slices. The two-different-external-globals
// counterexample stays rejected: see
// multi-tu-gate-g5-cellslice-global-negative.c.
//
// ORDERING LIMITATION (sound): the cell-slice promotion needs the callee's
// cell-slice SIGNATURE established when a call site is emitted, which today
// happens where the DEFINITION is imported. Main-first (definition before
// the companion's external call) promotes; companion-first — the external
// call ahead of the definition — conservatively falls back to the
// historical rejection (a missed optimization, never a miscompile; the
// REVERSED line pins it). Order-independent cell-slice signatures are a
// documented follow-up (design.md).
//
// RUN: emitrust-import-c %s %S/Inputs/multi-tu-gate-g5-cellslice-global-external-other.c | FileCheck %s
// RUN: not emitrust-import-c %S/Inputs/multi-tu-gate-g5-cellslice-global-external-other.c %s 2>&1 | FileCheck %s --check-prefix=REVERSED

// REVERSED: multi-tu-gate-g5-cellslice-global-external-other.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: passing a pointer into a global variable to a function

int region[8];

void region_fill(int *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    p[i] = i;
}

int main(void) {
  region_fill(region, 8);
  return region[3];
}

// `region` stays one shared global; `region_fill` takes a generic
// cell-slice and writes through it; every call site (both TUs) binds
// `region` via a global_cells region.
// CHECK-DAG: emitrust.global @region : !emitrust.array<8xi32>
// CHECK: func.func @region_fill(%{{.*}}: !emitrust.ref<!emitrust.cell_slice<i32>>, %{{.*}}: i32)
// CHECK: emitrust.cell_set %{{.*}} : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64, i32) -> ()
// CHECK: emitrust.global_cells @region {
// CHECK: func.call @region_fill(%{{.*}}, %{{.*}}) : (!emitrust.ref<!emitrust.cell_slice<i32>>, i32) -> ()
