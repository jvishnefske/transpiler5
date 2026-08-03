// REQUIRES: cargo
// FR-58 selective re-import of fact-starved items, end to end. SPIKE 2's
// constraint: an extern POINTER global cannot be typed by a solo import --
// the base+cursor decomposition needs the defining TU's shape -- so the
// shim's defer+recover import ledgers the accessing items (stubs) and, in
// the DEFINING shard, never materializes the cursor's index global at all
// (no use there). Merge-level synthesis is therefore impossible (measured:
// no shape to copy, no body IR to re-admit); the link step must RE-IMPORT.
// Mechanism pinned here: the link driver detects the fact-starved ledger
// entries (FR-57d metadata), finds the shard whose ITEM GRAPH defines the
// missing global (the graph records `node CURSOR kind=global def=1` even
// though the module does not), re-imports the group's SOURCES jointly --
// the artifact now records each TU's source path and import args -- runs
// the pinned pipeline, and replaces the member shards with the group
// module before the merge. The linked crate must be BYTE-IDENTICAL to the
// joint import of the same sources, and its stdout byte-identical to the
// clang-built native binary.
//
// Per-TU shards through the shim (defining TU first, matching the joint
// import's source order):
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-reimport-lib.c -o %t.lib.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
//
// The link re-imports the fact-starved group (observable on stderr) and
// builds the crate:
// RUN: emitrust-cc --link %t.lib.o %t.main.o -o %t.crate --crate-name link_reimport --build 2>%t.err
// RUN: FileCheck %s --check-prefix=REIMPORT < %t.err
// REIMPORT: link-time re-import:
// REIMPORT-SAME: link-reimport-lib.c
// REIMPORT-SAME: link-reimport-e2e.c
//
// Differential oracle against the clang-linked native binary:
// RUN: clang -std=c11 %S/Inputs/link-reimport-lib.c %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/link_reimport > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// Byte identity vs the joint import (the FR-58 acceptance oracle):
// RUN: emitrust-cc --emit=rust %S/Inputs/link-reimport-lib.c %s -o %t.joint.rs
// RUN: diff %t.joint.rs %t.crate/src/main.rs
//
// Sidecar inputs re-import identically:
// RUN: emitrust-cc --link %t.lib.o.emitrust.mlirbc %t.main.o.emitrust.mlirbc --emit=rust -o %t.sidecar.rs
// RUN: diff %t.joint.rs %t.sidecar.rs

extern int *cursor;

int printf(const char *, ...);

int read_cursor(void) { return *cursor; }

int main(void) {
  printf("v=%d\n", read_cursor());
  return 0;
}
