// FR-143: an owner method that IS in the crate must be counted as `ported`.
//
// The invariant this file pins: `emitrust-progress.json` is an artifact
// ABOUT the crate beside it, so every graph item whose emitted definition is
// in `src/lib.rs` reads `ported` -- including the ones FR-30 owner promotion
// moved out of module scope and into an `emitrust.impl` block.
//
// Why it needs its own test. Before FR-143, `collectEmittedSymbols`
// (tools/emitrust-cc/ProgressReport.cpp) iterated only
// `module.getBody()->getOperations()`: a method inside an `emitrust.impl` was
// never in the emitted set, so NO owner method had ever been counted as
// ported and every ledger this project ever recorded understated the
// promotion path. This source is the smallest total demonstration -- there is
// not one rejection anywhere in it, the crate builds, both items are in
// `src/lib.rs`, and the report used to say the project was 50% ported.
//
// The shape is deliberate: `fill` is `static`, takes the promoted buffer, and
// is therefore promoted into `impl OwnerMakeBuf` under its graph key
// `tu0_fill` -- the FR-40 guarantee that a node's key IS the emitted symbol
// is what makes the join sound at all, and it holds through promotion.
//
// Companion: test/Driver/incremental-owner-struct-reached.c pins the same
// reading on the shape where the OWNING function is stubbed.
//
// REQUIRES: cargo
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate --crate-type=lib \
// RUN:   --build 2>%t.err
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/lib.rs
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
// RUN: FileCheck %s --check-prefix=PORTING --input-file=%t.crate/PORTING.md
//
// Nothing in this project is unsupported, so the ledger has no rejection at
// all -- this is not a recovery story, it is a reporting one.
// RUN: FileCheck %s --check-prefix=WARN --allow-empty --input-file=%t.err
// WARN-NOT: warning:
// WARN-NOT: rejected top-level item

static int fill(char *s, long long v) {
  s[0] = (char)('0' + (v % 10));
  return 1;
}

int make(long long v) {
  char buf[32];
  fill(buf, v);
  return (int)buf[0];
}

// The method really is in the crate, inside the promoted owner's impl.
// RUST: pub struct OwnerMakeBuf {
// RUST: impl OwnerMakeBuf {
// RUST-NEXT: fn tu0_fill(&mut self, {{.*}}) -> i32 {

// Two graph items, two of them emitted, so the project is 100% ported. Before
// FR-143 this read `"ported": 1` / `"missing": 1` / `"ported_permille": 500`
// with `tu0_fill` blamed on `unreached-by-import` -- while its body sat in
// the very file checked above.
// JSON: "graph_items": 2
// JSON-NEXT: "ported": 2
// JSON-NEXT: "stubbed": 0
// JSON-NEXT: "dropped": 0
// JSON-NEXT: "missing": 0
// JSON-NEXT: "declared": 0
// JSON-NEXT: "off_graph_rejected": 0
// JSON-NEXT: "ported_permille": 1000
//
// No blocker is invented for an item that has none.
// JSON: "blockers": [],
// JSON-NEXT: "root_blockers": [],
// JSON: "symbol": "make"
// JSON-NEXT: "kind": "function"
// JSON-NEXT: "status": "ported"
// JSON: "symbol": "tu0_fill"
// JSON-NEXT: "kind": "function"
// JSON-NEXT: "status": "ported"
// JSON-NEXT: "color": "green"
// JSON: "blocker": ""

// The human artifact agrees with the machine one.
// PORTING: | ported | green | `make` | function |
// PORTING: | ported | green | `tu0_fill` | function |
