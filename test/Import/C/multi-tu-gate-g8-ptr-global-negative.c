// W3.1 multi-TU gate oracle (G8 NEGATIVE): `g` is defined and points at
// `A` in this file; the companion TU rebinds the SAME externally visible
// `g` to a DIFFERENT global (`B`, not `A`) and reads it. Even after W3.2
// merges `globalPtrFacts` across the whole project, a sound merge must
// detect this divergence: the single-global-region-base model
// (design.md CTS-P4) represents a pointer global's cursor as ONE stored
// i64 against ONE base object, with no representation for "the base
// depends on which TU last ran" (the existing single-TU "multi-object
// regions" rejection is the in-TU analogue). This test pins TODAY's
// rejection, which happens to already be correct for this shape (via
// `deferExternGlobal`'s unconditional check, not G8's own soleTU check —
// see multi-tu-gate-g8-ptr-global-shared-header.c); W3.2 must not
// accidentally start accepting it once both gates are relaxed.
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-gate-g8-ptr-global-negative-other.c 2>&1 | FileCheck %s

int A[4];
int *g = &A[0];

// CHECK: multi-tu-gate-g8-ptr-global-negative-other.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-typed global variable{{$}}
