// FR-58 owner-planning `soleTranslationUnit` divergence, pinned fixed. Two
// measured divergences between solo shards and the joint import, and their
// two fixes:
//
// (1) The shim's solo import used to pass `soleTranslationUnit=true`, so
// the Phase-4 owner/cell-slice planners promoted EXTERNALLY VISIBLE
// functions on the claim "all call sites are in this TU" -- a claim no
// shard can make. Measured: `sum4` below imported as a cell-slice
// parameter solo while the joint import (which sees this file's
// local-array call) refuses the promotion. Fixed: a defer-mode (shard)
// import never claims sole-TU, so the shard keeps the conservative slice
// model and the disqualified caller is stubbed with the SAME wording the
// joint import uses.
//
// (2) The accessing TU's call to the body-less `sum4` was shaped from the
// bare prototype (`&mut i32`) while the definition refines to a slice --
// a SILENT one-line divergence (the joint import treats the same shape as
// a located refinement rejection when the definition comes second). Fixed
// at the merge: an `extern_decl` function whose declared type disagrees
// with the defining shard's type is fact-starvation detected with
// whole-program information, and the link re-imports the pair jointly.
//
// Shim-built shards, definer first (the joint import's own refinement is
// order-sensitive; the link line mirrors the joint source order):
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-soletu-owners-lib.c -o %t.lib.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
//
// Pin (1): the shard does NOT promote -- no cell-slice anywhere, sum4
// keeps the conservative slice model, and the disqualified caller is
// ledgered with the joint import's own wording.
// RUN: emitrust-opt %t.lib.o.emitrust.mlirbc -o - | FileCheck %s --check-prefix=SHARD
// SHARD-NOT: cell_slice
// SHARD: emitrust.func @sum4(%{{.*}}: !emitrust.mut_ref<!emitrust.slice<i32>>)
// SHARD-NOT: cell_slice
//
// Pin (2): the link detects the signature-starved declaration, re-imports
// the pair, and the linked crate is BYTE-IDENTICAL to the recovering
// joint import of the same sources.
// RUN: emitrust-cc --link %t.lib.o %t.main.o --emit=rust -o %t.rs 2>%t.err
// RUN: FileCheck %s --check-prefix=REIMPORT < %t.err
// REIMPORT: link-time re-import:
// RUN: emitrust-cc --recover --emit=rust %S/Inputs/link-soletu-owners-lib.c %s -o %t.joint.rs 2>/dev/null
// RUN: diff %t.joint.rs %t.rs

int sum4(int *a);
int fill_and_sum(void);
int printf(const char *, ...);

int main(void) {
  int local[4];
  int i;
  for (i = 0; i < 4; i++)
    local[i] = 10 * i;
  printf("a=%d b=%d\n", fill_and_sum(), sum4(local));
  return 0;
}
