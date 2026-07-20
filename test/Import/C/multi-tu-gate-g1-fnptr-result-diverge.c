// W3.1 multi-TU gate oracle (G1 NEGATIVE): unlike
// multi-tu-gate-g1-fnptr-result-erasure.c (a genuinely sound shape the
// gate over-rejects today), this file's two translation units take the
// address of TWO DIFFERENT functions returning TWO DIFFERENT global
// bases (`get1`/&objA here, `get2`/&objB in the companion) through the
// SAME externally visible fn-ptr-typed declaration `p`. Even after W3.2's
// whole-program address-taken hoist, a real merge of both TUs' candidate
// sets must still reject this — not with today's blanket "function
// pointer result type" wording, but with the EXISTING disagreement
// diagnostic classifyFnPtrPointerResult already raises for a single TU
// with two divergent candidates (see pointers-return.c's MIXED case:
// "return sites disagree on the returned global base" /
// "...signature"). This test only pins TODAY's blanket rejection; W3.2
// must re-point it at that disagreement wording instead of silently
// accepting one of the two bases.
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-gate-g1-fnptr-result-diverge-other.c 2>&1 | FileCheck %s

struct S {
  int x;
};
struct S objA;

struct S *get1(void) { return &objA; }

struct S *(*p)(void) = &get1;
void rebind(void) { p = &get1; }

// CHECK: multi-tu-gate-g1-fnptr-result-diverge.c:{{[0-9]+}}:13: error: unsupported: function pointer result type
