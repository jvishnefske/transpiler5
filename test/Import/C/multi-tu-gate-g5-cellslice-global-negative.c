// W3.1 multi-TU gate oracle (G5 NEGATIVE): today, G4's preemptive poison
// of an externally visible function's OWN parameters already makes any
// cross-TU cell-slice class unreachable, so there is no whole-program
// union-find merge to counterexample yet (planCellSlices runs once per
// TU with fresh state — see planCellSlices' signature). This file pins
// the case a future whole-program merge (W3.2+) must still refuse even
// once G4/G5's blanket external-linkage exclusions are relaxed:
// `region_fill` is called here on the externally visible `region`
// (would-be G5 global) AND, per the companion, on a second externally
// visible array `other_region` from a different translation unit —
// TWO DIFFERENT global bases reachable through the SAME function
// parameter is the existing single-TU multi-base disqualification
// (test/Import/C/owners-fallback.c's TWOARR shape, ported to
// planCellSlices), and it must keep firing once whole-program
// visibility makes both call sites visible together, independent of
// the internal-linkage checks this wave's gates enforce today.
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-gate-g5-cellslice-global-negative-other.c 2>&1 | FileCheck %s --check-prefix=FIRST
// RUN: not emitrust-import-c %S/Inputs/multi-tu-gate-g5-cellslice-global-negative-other.c %s 2>&1 | FileCheck %s --check-prefix=SECOND

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

// FIRST: multi-tu-gate-g5-cellslice-global-negative.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: passing a pointer into a global variable to a function
// SECOND: multi-tu-gate-g5-cellslice-global-negative-other.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: passing a pointer into a global variable to a function
