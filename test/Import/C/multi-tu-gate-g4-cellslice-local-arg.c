// W3.1 multi-TU gate oracle (G4 NEGATIVE): `proc` looks, from THIS TU
// alone, like a plain all-global cell-slice candidate (its only visible
// call site here passes the global `G`). The companion TU calls the SAME
// externally visible `proc` with a LOCAL array (`loc`) instead — the
// shape G4's preemptive poison exists to guard against. Even after
// W3.2's whole-program hoist merges call-site facts across the project,
// a sound merge must still see the companion's local-array call site and
// keep `proc`'s parameter off the cell-slice path (a local has no Cell
// to back `&[Cell<T>]`); this is not a visibility problem the hoist
// removes, it is a genuine shape the all-global rule was never meant to
// admit. This test pins TODAY's rejection (the same "historical"
// staged-copy wording as the positive test), which must survive
// unchanged through W3.2.
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-gate-g4-cellslice-local-arg-other.c 2>&1 | FileCheck %s

int G[4];

void proc(int *p) { p[0] = 1; }

int main(void) {
  proc(G);
  return G[0];
}

// CHECK: multi-tu-gate-g4-cellslice-local-arg.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: passing a pointer into a global variable to a function
