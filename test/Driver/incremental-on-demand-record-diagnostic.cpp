// FR-115, second channel: a record rejected ON DEMAND still ledgers its own
// cause.
//
// The top-level recovery loop only sees declarations it walks. A class
// template instantiation is reached through `mapType` inside a function
// body -- `importRecord` with no recovery loop behind it -- so before
// FR-115 its rejection was memoized (`rejectedRecords`) and DISCARDED: the
// copy-ctor cause appeared in no ledger, the `BoxI32` graph node read
// `missing` with no diagnostic, and the only recorded trace was the user's
// downstream `struct 'BoxI32' was rejected` restatement. `importRecord` now
// captures its own failure (FR-112-shape ScopedDiagnosticHandler) and
// ledgers it under the graph key at the moment of memoization -- but ONLY
// for records the graph models: a nested record (`Inner` below) is not a
// node, its cause reaches the report through the enclosing record's entry,
// and a ledger row for it would be off-graph noise (WARN-NOT pins that).
// The captured diagnostics are re-emitted preserving the error/note
// structure, so recovery stderr and strict mode read exactly as before.
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=PORTING --input-file=%t.crate/PORTING.md
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
// RUN: not emitrust-cc --emit=crate %s -o %t.strict 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT

// --- Rejected instantiation (MOVE ctor -- the pre-W2.23 copy-ctor specimen
// --- is admitted now, same gate and wording), reached only from use_box's
// --- body. Graph node `BoxI32`.
template <typename T> struct Box {
  T v;
  Box(Box &&o) : v(o.v) {}
};
int use_box() {
  Box<int> *b = 0;
  return b ? b->v : 0;
}

// --- Rejected NESTED record: `Inner` is reached on demand from Outer's
// --- field walk, is not a graph node, and must NOT get a ledger row of its
// --- own; `Outer` (a node) carries the located cause.
struct Outer {
  struct Inner {
    int v;
    Inner(Inner &&o) : v(o.v) {}
  };
  Inner field;
};
int use_outer() {
  Outer *o = 0;
  return o ? o->field.v : 0;
}

// --- In subset, so the crate is not empty.
int fine(int a) { return a + 1; }

// `BoxI32` is ledgered ONCE with its real cause. The `Box` line below it is
// the template PATTERN's own top-level rejection -- a pre-existing off-graph
// residue (the pattern is not a graph item), pinned here AS-IS so a change
// to it is seen, not silently absorbed.
// WARN: warning: unsupported: copy/move/delegating constructor (recovered: item dropped)
// WARN: warning: unsupported: struct 'BoxI32' was rejected, so a type naming it cannot be imported (recovered: emitted an unimplemented!() stub with the mapped signature)
// WARN: recovered 5 rejected top-level items:
// WARN-NEXT: dropped 'BoxI32' [cxx-copy-ctor] unsupported: copy/move/delegating constructor
// WARN-NEXT: dropped 'Box' [cxx-copy-ctor] unsupported: copy/move/delegating constructor
// WARN-NEXT: stubbed 'use_box' [rejected-type-cascade] unsupported: struct 'BoxI32' was rejected, so a type naming it cannot be imported
// WARN-NEXT: dropped 'Outer' [other] unsupported: struct definition outside file or function scope
// WARN-NEXT: stubbed 'use_outer' [rejected-type-cascade] unsupported: struct 'Outer' was rejected, so a type naming it cannot be imported
// WARN-NOT: dropped 'Inner'

// The instantiation's node is a real dropped row with the cause; nothing is
// `missing`; the only off-graph entry is the pattern residue.
// PORTING: ## Project items
// PORTING: | dropped | red | `BoxI32` | record | cxx-copy-ctor | BoxI32 | cxx-copy-ctor | unsupported: copy/move/delegating constructor |
// PORTING: | dropped | red | `Outer` | record | other | Outer | other | unsupported: struct definition outside file or function scope |
// PORTING: ## Rejected items outside the item graph
// PORTING: | dropped | red | `Box` |

// JSON: "graph_items": 5
// JSON-NEXT: "ported": 1
// JSON-NEXT: "stubbed": 2
// JSON-NEXT: "dropped": 2
// JSON-NEXT: "missing": 0
// JSON-NEXT: "declared": 0
// JSON-NEXT: "off_graph_rejected": 1
// JSON: "symbol": "BoxI32"
// JSON: "status": "dropped"
// JSON: "blocker": "cxx-copy-ctor"
// JSON-NEXT: "diagnostic": "unsupported: copy/move/delegating constructor"
// JSON: "symbol": "Outer"
// JSON: "status": "dropped"
// JSON: "blocker": "other"
// JSON-NEXT: "diagnostic": "unsupported: struct definition outside file or function scope"
// JSON: "off_graph_items": [
// JSON: "symbol": "Box"

// The capture-replay in importRecord preserves strict mode byte-for-byte:
// one located error, no restructured notes, no duplicate.
// STRICT: error: unsupported: copy/move/delegating constructor
// STRICT-NOT: error: unsupported: copy/move/delegating constructor
