// REQUIRES: cargo
// FR-158 phase 2: the link must not depend on LINK-LINE ORDER.
//
// FR-58's remedy for a signature-starved declaration is a joint re-import
// of the group's sources, and that re-import feeds the sources to the
// importer in LINK-LINE order. When a caller TU precedes the definition,
// the importer shapes the call from the bare prototype and then REFUSES to
// refine the signature under it ("function 'note' was called as ... before
// its definition refined the signature ... (cross-TU pointer-parameter
// classification)"); under the recovering import that DROPS the definition,
// and the merge then fails with "unresolved external 'note' at link" for a
// symbol the original shards define perfectly well. Measured at HEAD:
//   emitrust-cc --link main.o def.o use.o  -> clean crate, matches native
//   emitrust-cc --link main.o use.o def.o  -> unresolved external 'note'
//
// The fix keeps the ORIGINAL shards whenever a group's re-import stops
// defining a symbol its members defined; the phase 1 merge-level slice
// reconciliation then handles the divergence order-free. This file pins
// BOTH orders: each must build and each must produce stdout byte-identical
// to the clang-built native. `line` derives from `argc` so no constant fold
// can stand in for the argument.
//
// Note the contrast with link-slice-model-e2e.c: here every TU is compiled
// with the SAME import args, which is exactly what lets the re-import fire
// at all -- the definition-first order below is served by the re-import and
// the caller-first order by the phase 1 merge path.
//
// Per-TU shards through the shim, identical import args:
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-slice-order-def.c -o %t.def.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-slice-order-use.c -o %t.use.o
//
// The definition-first order: the joint re-import succeeds and stands in.
// RUN: emitrust-cc --link %t.main.o %t.def.o %t.use.o -o %t.dfirst --crate-name link_slice_order --build 2>%t.dfirst.err
// RUN: FileCheck %s --check-prefix=DFIRST < %t.dfirst.err
// DFIRST: link-time re-import:
// DFIRST-NOT: keeping the original shards
//
// The caller-first order: the re-import loses the definition, the group is
// discarded, and the original shards reconcile at merge time.
// RUN: emitrust-cc --link %t.main.o %t.use.o %t.def.o -o %t.ufirst --crate-name link_slice_order --build 2>%t.ufirst.err
// RUN: FileCheck %s --check-prefix=UFIRST < %t.ufirst.err
// UFIRST: warning: link-time re-import of {{.*}} no longer defines 'note'; keeping the original shards
//
// Differential oracle against the clang-linked native binary, BOTH orders,
// with line = 7 * argc over three argument counts:
// RUN: clang -std=c11 %s %S/Inputs/link-slice-order-def.c %S/Inputs/link-slice-order-use.c -o %t.native
// RUN: %t.native > %t.n0.out
// RUN: %t.dfirst/target/release/link_slice_order > %t.d0.out && diff %t.n0.out %t.d0.out
// RUN: %t.ufirst/target/release/link_slice_order > %t.u0.out && diff %t.n0.out %t.u0.out
// RUN: %t.native a > %t.n1.out
// RUN: %t.dfirst/target/release/link_slice_order a > %t.d1.out && diff %t.n1.out %t.d1.out
// RUN: %t.ufirst/target/release/link_slice_order a > %t.u1.out && diff %t.n1.out %t.u1.out
// RUN: %t.native a b > %t.n2.out
// RUN: %t.dfirst/target/release/link_slice_order a b > %t.d2.out && diff %t.n2.out %t.d2.out
// RUN: %t.ufirst/target/release/link_slice_order a b > %t.u2.out && diff %t.n2.out %t.u2.out

int run(int k);

int main(int argc, char **argv) {
  return run(7 * argc);
}
