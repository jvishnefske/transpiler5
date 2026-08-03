// REQUIRES: cargo
// FR-59 workspace partitioning, end to end: a 3-TU project whose sources
// live in two subsystem directories plus this main file partitions -- under
// `--link --emit=crate --partition` -- into a Cargo WORKSPACE of two
// library crates (one per source directory) and one binary crate, wired by
// path dependencies and `use <dep>::*;` imports, with cross-crate
// references flowing through FR-51's export rules (pub functions, pub
// types; the file-static in subsys_a stays private behind its per-TU tag).
// The dependency edges come from the shards' FR-57d item-graph texts, the
// same indexed source the selective re-import uses. The workspace builds
// with one `cargo build` at the root, and its stdout is byte-identical to
// the clang-native binary AND to the unpartitioned single-crate link of
// the same objects -- partitioning must never change behavior, only
// packaging.
//
// Shim-built shards, dependency order (a, then b which calls a, then main):
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-ws/subsys_a/geom.c -o %t.a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-ws/subsys_b/calc.c -o %t.b.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
//
// Partitioned workspace, built at the root:
// RUN: rm -rf %t.ws
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o -o %t.ws --crate-name link_ws --emit=crate --partition --build
//
// Layout: a virtual workspace root plus three member crates.
// RUN: ls %t.ws/Cargo.toml %t.ws/subsys_a/src/lib.rs %t.ws/subsys_b/src/lib.rs %t.ws/link_ws/src/main.rs
// RUN: FileCheck %s --check-prefix=ROOT < %t.ws/Cargo.toml
// ROOT: [workspace]
// ROOT: members = ["subsys_a", "subsys_b", "link_ws"]
//
// The dependent crates carry path dependencies and glob imports:
// RUN: FileCheck %s --check-prefix=BDEP < %t.ws/subsys_b/Cargo.toml
// BDEP: [dependencies]
// BDEP: subsys_a = { path = "../subsys_a" }
// RUN: FileCheck %s --check-prefix=BUSE < %t.ws/subsys_b/src/lib.rs
// BUSE: use subsys_a::*;
//
// The subsys_a file-static stays private (per-TU tagged, never pub):
// RUN: not grep "pub fn tu" %t.ws/subsys_a/src/lib.rs
//
// Differential oracle against the clang-native binary:
// RUN: clang -std=c11 %S/Inputs/link-ws/subsys_a/geom.c %S/Inputs/link-ws/subsys_b/calc.c %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.ws/target/release/link_ws > %t.ws.out
// RUN: diff %t.native.out %t.ws.out
//
// Partitioning changes packaging, never behavior: the unpartitioned
// single-crate link of the same objects runs byte-identically.
// RUN: rm -rf %t.single
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o -o %t.single --crate-name link_ws --emit=crate --build
// RUN: %t.single/target/release/link_ws > %t.single.out
// RUN: diff %t.single.out %t.ws.out

#include "Inputs/link-ws/subsys_a/pair.h"

int calc(int n);
int pair_sum(struct Pair p);

int printf(const char *, ...);

int main(void) {
  struct Pair q;
  q.x = 5;
  q.y = 2;
  printf("calc=%d direct=%d\n", calc(3), pair_sum(q));
  return 0;
}
