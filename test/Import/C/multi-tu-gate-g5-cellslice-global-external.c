// W3.1 multi-TU gate oracle (G5) — predicted failure #5 (external-linkage
// region APIs): planCellSlices (CTS-P10) requires every global base of a
// cell-slice class to be internal-linkage unless this TU is the whole
// program (ImportC.cpp:2387) — another TU's writeback through the same
// externally visible array could observe or defeat the Cell-coherence
// invariant cell-slices depend on (design.md's "coherence trap": direct
// global reads and Cell get/set must hit the same storage). `region` here
// is a realistic exported region-API shape (a global buffer plus an
// externally visible function operating on it, called from `main` in
// this TU AND from the companion TU) — exactly CTS-P6/CTS-P10's target
// shape, but with external linkage on both the array and the function.
//
// Like G4 (and unlike G3's owners), this is a real correctness-visible
// rejection, not a silent optimization loss: without a cell-slice class,
// `region_fill(region, 8)` hits the pre-CTS-P10 "passing a pointer into a
// global variable to a function" rejection.
//
// W3.2 will flip the `not` RUN line below to `emitrust-import-c ... |
// FileCheck` (asserting `!emitrust.ref<!emitrust.cell_slice<i32>>` and
// `emitrust.global_cells @region`) once planCellSlices' global-base
// internal-linkage check is relaxed by a whole-program merge.
//
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-gate-g5-cellslice-global-external-other.c 2>&1 | FileCheck %s --check-prefix=FIRST
// RUN: not emitrust-import-c %S/Inputs/multi-tu-gate-g5-cellslice-global-external-other.c %s 2>&1 | FileCheck %s --check-prefix=SECOND

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

// Whichever TU is imported first hits its own call site's rejection
// first; each order pins a different located diagnostic.
// FIRST: multi-tu-gate-g5-cellslice-global-external.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: passing a pointer into a global variable to a function
// SECOND: multi-tu-gate-g5-cellslice-global-external-other.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: passing a pointer into a global variable to a function
