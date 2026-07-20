// W3.1 multi-TU gate oracle (G4): planCellSlices (CTS-P10) poisons an
// externally visible function's OWN pointer parameters preemptively
// (ImportC.cpp:2278) — another TU could call it with a local argument
// this TU never sees, and a cell-slice parameter has no representation
// for a non-global (Cell-less) base. Poison is UNION-FIND-WIDE: `pub_fn`
// forwards its parameter to `helper` (a plain forwarded-argument call
// edge unites both parameters into ONE class — see planCellSlices'
// `forEachDataPointerCallArg`), so `pub_fn`'s preemptive poison cascades
// to `helper` too even though `helper` alone is internal-linkage and
// would otherwise qualify under G6. UNLIKE G3 (owners), this
// disqualification does not fall back to a working, unoptimized
// lowering: with no cell-slice class, `pub_fn(A)` in `main` hits the
// PRE-CTS-P10 "passing a pointer into a global variable to a function"
// rejection (design.md CTS-P4's "historical staged-copy rejection",
// still real for any call site the all-global cell-slice class does not
// admit) — a genuine correctness-visible regression, not a pure
// optimization loss.
//
// W3.2 will flip the `not` RUN line below to `emitrust-import-c ... |
// FileCheck` (asserting cell_get/cell_set IR on both `pub_fn` and
// `helper`) once planCellSlices' poison set and union-find classes are
// merged across every AST in the project.
//
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-gate-g4-cellslice-poison-other.c 2>&1 | FileCheck %s

int A[4];

static int helper(int *a) { return a[0]; }

int pub_fn(int *a) { return helper(a); }

int main(void) {
  int i;
  for (i = 0; i < 4; i++)
    A[i] = i + 1;
  return pub_fn(A);
}

// CHECK: multi-tu-gate-g4-cellslice-poison.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: passing a pointer into a global variable to a function
