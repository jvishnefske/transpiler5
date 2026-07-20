// W3.1 multi-TU gate oracle (G6): planCellSlices (CTS-P10) requires the
// OWNING FUNCTION of every cell-slice parameter to be internal linkage
// unless this TU is the whole program (ImportC.cpp:2417) — an externally
// visible function's parameter could be called from an unseen TU with a
// non-global argument, same underlying concern as G4's poison but
// checked again per-class here. Isolated from G5: `A` (the global base)
// is INTERNAL linkage in this file, only `sum4` (the owning function) is
// externally visible and called from the companion TU too — on the
// companion's OWN internal-linkage global, so once a whole-program merge
// exists this is a genuinely sound shape to promote (contrast with
// multi-tu-gate-g6-cellslice-fn-negative.c, whose companion passes a
// LOCAL array instead).
//
// Real correctness-visible rejection (not a silent fallback): without a
// cell-slice class, `sum4(A)` hits the pre-CTS-P10 "passing a pointer
// into a global variable to a function" rejection.
//
// W3.2 will flip the `not` RUN line below to `emitrust-import-c ... |
// FileCheck` once the owning-function internal-linkage check is relaxed
// by a whole-program merge of call-site facts.
//
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-gate-g6-cellslice-fn-external-other.c 2>&1 | FileCheck %s

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

// CHECK: multi-tu-gate-g6-cellslice-fn-external.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: passing a pointer into a global variable to a function
