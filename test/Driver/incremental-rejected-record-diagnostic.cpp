// FR-115: a rejected item's OWN graph row carries its blocker and diagnostic.
//
// The defect this exists to prevent: the recovery ledger used to key a
// rejection by its raw C spelling (`declLedgerName`), while the FR-40 item
// graph keys the same declaration by its EMITTED name -- `recordRustName`
// for records (namespace-prefixed), `cGlobalSymbolName` for globals
// (idiomatic SCREAMING_SNAKE rename), `cFunctionSymbolName(func, tuTag)`
// for functions (namespace prefix, TU tag). Whenever the two spellings
// diverged, the join at report time missed: the rejection fell into the
// off-graph table under a name no graph node owns, the node itself read
// `status: missing` with `blocker: ""`, `diagnostic: ""`, and every USE
// site restated `struct 'X' was rejected` -- symptoms ranked in the blocker
// table while the causes went uncounted (measured corpus-wide: 6193 of 8851
// graph items were `missing` with no diagnostic at all). The fix keys the
// ledger by the graph's own vocabulary (`graphItemSymbol`, the derivation
// `frontierExcludedSymbol` already mirrored from
// `ItemGraphBuilder::collectItems`), falling back to the old spelling only
// for declarations the graph does not model.
//
// One TU, all four diverging-name families: a namespaced record, a global
// of its type under idiomatic rename, a namespaced function, and a
// keyword-spelled function (`use` -> `use_`, the keyword-avoidance mangle).
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=PORTING --input-file=%t.crate/PORTING.md
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
//
// Strict mode is byte-compatible with what it always printed: the ledger
// keying change must not reword or restructure the located rejection.
// RUN: not emitrust-cc --emit=crate %s -o %t.strict 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT
//
// FR-115 is a ledger-KEYING and reporting change: the emitted Rust is
// byte-identical to what a plain --recover run of the same input produces.
// RUN: emitrust-cc --emit=crate --recover %s -o %t.recover.crate 2>/dev/null
// RUN: diff -r %t.crate/src %t.recover.crate/src

// --- The cause: rejected on its copy constructor. Graph key `NsGeoGadget`,
// --- raw spelling `Gadget` -- the divergence that used to lose the join.
namespace geo {
struct Gadget {
  int v;
  Gadget() : v(0) {}
  Gadget(const Gadget &other) : v(other.v) {}
};
}

// --- A global of the rejected type: graph key `THE_BAD_GLOBAL` (idiomatic
// --- rename), raw spelling `the_bad_global`.
geo::Gadget the_bad_global;

// --- A namespaced function DROPPED (by-value parameter of the rejected
// --- type, so the stub retry cannot map the signature either): graph key
// --- `ns_math_bad`, raw spelling `bad`.
namespace math {
int bad(geo::Gadget g) { return g.v; }
}

// --- A stubbable user, so the cascade blocker is represented too.
int use_gadget() {
  geo::Gadget *g = 0;
  return g ? g->v : 0;
}

// --- A keyword-spelled user: raw spelling `use`, graph key `use_` (the
// --- keyword-avoidance mangle) -- the join-miss family that needs no
// --- namespace or linkage rename at all.
int use(int n) {
  geo::Gadget *g = 0;
  return g ? g->v : 0;
}

// --- In subset, so the crate is not empty.
int fine(int a) { return a + 1; }

// The recovery summary names every item by its graph key -- and lists each
// rejection exactly ONCE: `importRecord` now ledgers a graph-modelled record
// itself, and the top-level recovery loop must not record it a second time
// (the count of 5 and the tabulation's `cxx-copy-ctor 1` pin that).
// WARN: warning: unsupported: copy/move/delegating constructor (recovered: item dropped)
// WARN: recovered 5 rejected top-level items:
// WARN-NEXT: dropped 'NsGeoGadget' [cxx-copy-ctor] unsupported: copy/move/delegating constructor
// WARN-NEXT: dropped 'THE_BAD_GLOBAL' [rejected-type-cascade] unsupported: struct 'NsGeoGadget' was rejected, so a type naming it cannot be imported
// WARN-NEXT: dropped 'ns_math_bad' [rejected-type-cascade] unsupported: struct 'NsGeoGadget' was rejected, so a type naming it cannot be imported
// WARN-NEXT: stubbed 'use_gadget' [rejected-type-cascade] unsupported: struct 'NsGeoGadget' was rejected, so a type naming it cannot be imported
// WARN-NEXT: stubbed 'use_' [rejected-type-cascade] unsupported: struct 'NsGeoGadget' was rejected, so a type naming it cannot be imported
// WARN: blocker tabulation (recovered items by tag):
// WARN-NEXT: cxx-copy-ctor 1
// WARN-NEXT: rejected-type-cascade 4

// Every rejected row is a GRAPH row with a real blocker and diagnostic, the
// record's cascade is credited to the copy constructor (FR-49 chains through
// the joined record node), and the off-graph table does not exist.
// PORTING: ## Project items
// PORTING: | dropped | red | `NsGeoGadget` | record | copy-move-constructor | NsGeoGadget | cxx-copy-ctor | unsupported: copy/move/delegating constructor |
// PORTING-NEXT: | dropped | red | `THE_BAD_GLOBAL` | global | copy-move-constructor | THE_BAD_GLOBAL -> NsGeoGadget | rejected-type-cascade | unsupported: struct 'NsGeoGadget' was rejected, so a type naming it cannot be imported |
// PORTING-NEXT: | dropped | red | `ns_math_bad` | function | copy-move-constructor | ns_math_bad -> NsGeoGadget | rejected-type-cascade | unsupported: struct 'NsGeoGadget' was rejected, so a type naming it cannot be imported |
// PORTING-NEXT: | stubbed | yellow | `use_` | function | copy-move-constructor | use_ -> NsGeoGadget | rejected-type-cascade | unsupported: struct 'NsGeoGadget' was rejected, so a type naming it cannot be imported |
// PORTING-NEXT: | stubbed | yellow | `use_gadget` | function | copy-move-constructor | use_gadget -> NsGeoGadget | rejected-type-cascade | unsupported: struct 'NsGeoGadget' was rejected, so a type naming it cannot be imported |
// PORTING-NEXT: | ported | green | `fine` | function |
// PORTING-NOT: ## Rejected items outside the item graph

// No item is `missing`, nothing lands off-graph, the record's own row
// carries the cause, and the JSON publishes the FR-49 root and chain for
// the exact rows that used to read blank (root_blocker "", blame_chain []).
// JSON: "graph_items": 6
// JSON-NEXT: "ported": 1
// JSON-NEXT: "stubbed": 2
// JSON-NEXT: "dropped": 3
// JSON-NEXT: "missing": 0
// JSON-NEXT: "declared": 0
// JSON-NEXT: "off_graph_rejected": 0
// JSON: "symbol": "NsGeoGadget"
// JSON: "status": "dropped"
// JSON: "blocker": "cxx-copy-ctor"
// JSON-NEXT: "diagnostic": "unsupported: copy/move/delegating constructor"
// JSON-NEXT: "root_blocker": "copy-move-constructor"
// JSON-NEXT: "attributed_via": ""
// JSON-NEXT: "blame_chain": ["NsGeoGadget"]
// JSON: "symbol": "THE_BAD_GLOBAL"
// JSON: "status": "dropped"
// JSON: "blocker": "rejected-type-cascade"
// JSON-NEXT: "diagnostic": "unsupported: struct 'NsGeoGadget' was rejected, so a type naming it cannot be imported"
// JSON-NEXT: "root_blocker": "copy-move-constructor"
// JSON-NEXT: "attributed_via": ""
// JSON-NEXT: "blame_chain": ["THE_BAD_GLOBAL", "NsGeoGadget"]
// JSON: "symbol": "ns_math_bad"
// JSON: "status": "dropped"
// JSON: "blocker": "rejected-type-cascade"
// JSON: "symbol": "use_"
// JSON: "status": "stubbed"
// JSON: "blocker": "rejected-type-cascade"
// JSON-NEXT: "diagnostic": "unsupported: struct 'NsGeoGadget' was rejected, so a type naming it cannot be imported"
// JSON-NEXT: "root_blocker": "copy-move-constructor"
// JSON-NEXT: "attributed_via": ""
// JSON-NEXT: "blame_chain": ["use_", "NsGeoGadget"]
// JSON: "off_graph_items": []

// STRICT: error: unsupported: copy/move/delegating constructor
