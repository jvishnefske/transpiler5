// W3.1 multi-TU gate oracle (G1 NEGATIVE): unlike
// multi-tu-gate-g1-fnptr-result-erasure.c (a genuinely sound shape the
// gate over-rejects today), this file's two translation units take the
// address of TWO DIFFERENT functions returning TWO DIFFERENT global
// bases (`get1`/&objA here, `get2`/&objB in the companion) through the
// SAME externally visible fn-ptr-typed declaration `p`. A real merge of
// both TUs' candidate sets must still reject this. W3.4 G1's whole-program
// candidate-completeness fact reports TWO distinct TUs taking the address
// of a function returning `struct S *` (`get1` here, `get2` in the
// companion), so the gate keeps its blanket rejection — the SOUND outcome.
// The PRECISE cross-TU disagreement wording (mirroring pointers-return.c's
// MIXED "return sites disagree on the returned global base") is DEFERRED:
// it needs the full erased-base substrate (each candidate's base symbol
// resolved whole-program), whereas same-TU divergence still uses the
// precise wording via the per-TU classifier. The blanket reject is sound
// and honest for the cross-TU case (design.md FR-34). This test pins that
// blanket rejection.
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-gate-g1-fnptr-result-diverge-other.c 2>&1 | FileCheck %s

struct S {
  int x;
};
struct S objA;

struct S *get1(void) { return &objA; }

struct S *(*p)(void) = &get1;
void rebind(void) { p = &get1; }

// CHECK: multi-tu-gate-g1-fnptr-result-diverge.c:{{[0-9]+}}:13: error: unsupported: function pointer result type
