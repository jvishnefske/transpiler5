// FR-44: emitrust-cc --emit=crate --incremental emits the crate a partially
// supported project CAN produce, plus the two progress artifacts that say
// what is missing: PORTING.md (human) and emitrust-progress.json (machine).
//
// This input has one item of each outcome the report distinguishes:
//   * `ported` -- `add` and the record `Pair` are fully in the subset;
//   * `stubbed` -- `scaled`'s signature maps but its volatile local does not,
//     so an unimplemented!() stub with that signature stands in for it;
//   * `dropped` -- `widen` returns a `_Complex double`, which has no mapping
//     at all, so nothing is emitted for it (and `c_main`, whose only fault is
//     calling it, is stubbed in turn).
// The DENOMINATOR is the FR-40 item graph, so `widen` is counted even though
// the emitted module contains no trace of it -- which is the whole point of
// joining the graph with the ledger rather than counting emitted symbols.
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate 2>%t.err
// RUN: FileCheck %s --check-prefix=PORTING --input-file=%t.crate/PORTING.md
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
//
// --incremental IMPLIES --recover, so the very same compile without it is a
// hard error and produces no crate at all.
// RUN: not emitrust-cc --emit=crate %s -o %t.strict.crate 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT
//
// The artifacts are the ONLY difference: --incremental --recover and --recover
// alone render byte-identical Rust.
// RUN: emitrust-cc --emit=crate --recover %s -o %t.recover.crate 2>/dev/null
// RUN: diff %t.crate/src/main.rs %t.recover.crate/src/main.rs
// RUN: diff %t.crate/Cargo.toml %t.recover.crate/Cargo.toml
//
// The flag is crate-only; every other --emit rejects it up front.
// RUN: not emitrust-cc --emit=rust --incremental %s -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=ONLYCRATE

struct Pair {
  int lo;
  int hi;
};

int add(int a, int b) { return a + b; }

int scaled(int x) {
  volatile int v = x;
  return v * 2;
}

_Complex double widen(int x) { return (_Complex double)x; }

int main(void) {
  struct Pair p;
  p.lo = add(20, 22);
  p.hi = scaled(p.lo);
  widen(p.hi);
  return p.lo - 42;
}

// The headline fraction, the per-status tally, and the ranked blocker table.
// PORTING: # Porting status: `incremental`
// PORTING: **2 of 5 items ported (40.0%).**
// PORTING: | ported | 2 |
// PORTING-NEXT: | stubbed | 2 |
// PORTING-NEXT: | dropped | 1 |
// PORTING-NEXT: | missing | 0 |
// PORTING-NEXT: | declared | 0 |
// FR-49: this C project has no cascade to attribute. The FR-41 probe
// deliberately UNDER-approximates and leaves `_Complex` and `volatile`
// Green, so no item here has a poison chain and every item is its own root
// -- which makes the two tables identical, tag for tag and count for count.
// That is the documented degradation, not a special case, and it is what
// keeps the C corpus's ranked backlog exactly as it was before FR-49.
// PORTING: ## Root blockers, most items first
// PORTING: | other | 3 |
// PORTING: ## Direct blockers, as reported
// PORTING: | other | 3 |
//
// Items sort unported-first and cluster by blocker, so the table reads as a
// work queue: dropped, then stubbed, then ported, alphabetical within each.
// Each rejected row carries its root blocker and the one-element blame chain
// that says the item is its own root; a ported row carries neither.
// PORTING: ## Project items
// PORTING: | dropped | red | `widen` | function | other | widen | other |
// PORTING: | stubbed | yellow | `c_main` | function | other | c_main | other |
// PORTING: | stubbed | yellow | `scaled` | function | other | scaled | other |
// PORTING: | ported | green | `Pair` | record | - | - | - | - |
// PORTING: | ported | green | `add` | function | - | - | - | - |

// JSON: "schema": "emitrust-progress/1"
// JSON: "crate": "incremental"
// JSON: "denominator_source": "item-graph"
// JSON: "graph_items": 5
// JSON-NEXT: "ported": 2
// JSON-NEXT: "stubbed": 2
// JSON-NEXT: "dropped": 1
// JSON-NEXT: "missing": 0
// JSON-NEXT: "declared": 0
// JSON-NEXT: "off_graph_rejected": 0
// JSON-NEXT: "ported_permille": 400
//
// FR-49's additions are strictly additive, so the schema id is unchanged and
// `blockers` still carries the DIRECT tally; `root_blockers` is the new key
// beside it, with the same contents here because nothing cascades.
// JSON: "blockers": [
// JSON-NEXT: { "tag": "other", "count": 3 }
// JSON-NEXT: ],
// JSON-NEXT: "root_blockers": [
// JSON-NEXT: { "tag": "other", "count": 3 }
// JSON-NEXT: ],
// JSON: "symbol": "widen"
// JSON: "status": "dropped"
// JSON: "color": "red"
// JSON: "blocker": "other"
// JSON: "root_blocker": "other"
// JSON-NEXT: "attributed_via": ""
// JSON-NEXT: "blame_chain": ["widen"]

// STRICT: error: unsupported: volatile-qualified type

// ONLYCRATE: error: --incremental is only valid with --emit=crate
