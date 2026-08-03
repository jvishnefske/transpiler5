// FR-57d + FR-58: the per-TU artifact's rejection-ledger entries and
// item-graph shard SURFACE at the link step, attributed to their shard --
// the fact FR-58's selective re-import needs to identify fact-starved items
// per TU without re-parsing any C. This test pins three things: (1) a
// rejection recovered inside one TU's shim import is reported by
// `emitrust-cc --link` with the SHARD it came from named, in the same
// summary format the joint import prints; (2) the metadata survives `ar`
// collection -- an archived member's ledger surfaces identically and the
// emitted Rust is byte-identical to the loose-object link; (3)
// `--emit=item-graph` under `--link` dumps each shard's stored item-graph,
// per shard in link-line order (NOT merged -- shard-graph merging is
// FR-58's own open item), so the graphs are obtainable from the artifacts
// alone.
//
// Shim-built objects, the library TU carrying a stubbed rejection:
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-merge-rejections-lib.c -o %t.lib.o
//
// The link surfaces the library shard's ledger entry, shard-attributed:
// RUN: emitrust-cc --link %t.main.o %t.lib.o --emit=rust -o %t.rs 2>%t.err
// RUN: FileCheck %s --check-prefix=SURFACE < %t.err
// SURFACE: shard '{{.*}}.lib.o': recovered 1 rejected top-level item:
// SURFACE: stubbed 'lib_rejected'
// SURFACE-SAME: unsupported: volatile-qualified type
//
// The stub itself reached the merged crate (the rejection cost the item,
// never the shard):
// RUN: FileCheck %s --check-prefix=RUST < %t.rs
// RUST: fn lib_good(
// RUST: fn lib_rejected(
// RUST: unimplemented!
//
// `ar` survival: the archived member's ledger surfaces the same way and
// the merged Rust is byte-identical to the loose-object link:
// RUN: rm -f %t.lib.a
// RUN: llvm-ar rcs %t.lib.a %t.lib.o
// RUN: emitrust-cc --link %t.main.o %t.lib.a --emit=rust -o %t.ar.rs 2>%t.ar.err
// RUN: FileCheck %s --check-prefix=SURFACEAR < %t.ar.err
// SURFACEAR: shard '{{.*}}.lib.a({{.*}}.lib.o)': recovered 1 rejected top-level item:
// RUN: diff %t.rs %t.ar.rs
//
// Per-shard item-graph dump from the artifacts alone:
// RUN: emitrust-cc --link %t.main.o %t.lib.o --emit=item-graph -o %t.graph
// RUN: FileCheck %s --check-prefix=GRAPH < %t.graph
// GRAPH: shard 0 '{{.*}}.main.o'
// GRAPH: node c_main kind=function def=1
// GRAPH: node lib_good kind=function def=0
// GRAPH: edge c_main -> lib_good kind=Calls
// GRAPH: shard 1 '{{.*}}.lib.o'
// GRAPH: node lib_good kind=function def=1
// GRAPH: node lib_rejected kind=function def=1

int lib_good(int x);

int main(void) { return lib_good(41); }
