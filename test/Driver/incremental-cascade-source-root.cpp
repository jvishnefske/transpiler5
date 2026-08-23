// FR-126 (channel 1): a rejected-type-cascade is credited to the rejected
// TYPE's OWN root, not left as a one-hop chain that names the symptom.
//
// The defect this exists to prevent: `use_box` below is stubbed with the
// restatement "struct 'BoxI32' was rejected, so a type naming it cannot be
// imported", and before FR-126 its report row read root_blocker
// `rejected-type-cascade`, attributed_via "", blame_chain ['use_box'] --
// the cascade stopped one hop short of the answer because the rejected
// TYPE's own root was never recorded anywhere the report could join. The
// FR-41 coloring cannot supply it here: template instantiations are
// deliberately screened out of the admissibility probe (no
// per-instantiation key, see ItemColoring.cpp isTemplated), so `BoxI32`
// has no color chain, and the on-demand FR-115 ledger row holding its real
// cause (`cxx-copy-ctor`) was never joined to the cascade. Measured on the
// 110-unit C++ corpus re-sweep, 79.7% of all cascade rows had that
// self-rooted shape (2218 of 2783); with this change it is 5.2%.
//
// The mechanism: the importer records the rejected type's graph key at the
// cascade emit site (RejectedItem::cascadeSourceSymbol, threaded through
// the verbatim message), and the report resolves it -- through the source's
// coloring when it has one, else through the source's own ledger row,
// transitively -- ONLY when the coloring produced no chain of its own, so
// every already-resolving cascade keeps its byte-identical row.
//
// The enum twin is pinned too: `importEnum`'s cascade records its source
// the same way, and the top-level enum recovery already ledgers the enum's
// own cause, so `use_enum` roots at `enum-def-rejected` without an
// FR-115-shape capture inside importEnum.
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate 2>/dev/null
// RUN: FileCheck %s --check-prefix=PORTING --input-file=%t.crate/PORTING.md
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
//
// FR-126 is a REPORTING change: the emitted Rust is byte-identical to what
// a plain --recover run of the same input produces.
// RUN: emitrust-cc --emit=crate --recover %s -o %t.recover.crate 2>/dev/null
// RUN: diff -r %t.crate/src %t.recover.crate/src

// --- The rejected type: an instantiation reached ON DEMAND from use_box's
// --- body (graph node `BoxI32`), dropped for its copy constructor. Its
// --- own cause is in the ledger (FR-115), not in the coloring.
template <typename T> struct Box {
  T v;
  Box(const Box &o) : v(o.v) {}
};
int use_box() {
  Box<int> *b = 0;
  return b ? b->v : 0;
}

// --- The enum twin: rejected for a keyword enumerator, then named by
// --- use_enum. Graph node `Bad` carries `enum-def-rejected` itself.
enum Bad { fn = 0, ok = 1 };
int use_enum() { Bad b = ok; return (int)b; }

// --- In subset, so the crate is not empty.
int fine(int a) { return a + 1; }

// The root table counts the cascades under their CAUSES: the two stubs and
// the off-graph pattern residue join BoxI32 under its copy-ctor tag, and
// use_enum joins Bad under the enum rejection. `rejected-type-cascade`
// appears in NO root row.
// PORTING: ## Root blockers, most items first
// PORTING: | cxx-copy-ctor | 3 |
// PORTING: | enum-def-rejected | 2 |
//
// The direct table still carries the cascade tag -- the symptom stays
// visible, it just no longer ranks.
// PORTING: ## Direct blockers, as reported
// PORTING: | cxx-copy-ctor | 2 |
// PORTING: | rejected-type-cascade | 2 |
// PORTING: | enum-def-rejected | 1 |
//
// Each cascade's chain now steps THROUGH the rejected type to its root.
// PORTING: ## Project items
// PORTING: | dropped | red | `BoxI32` | record | cxx-copy-ctor | BoxI32 | cxx-copy-ctor | unsupported: copy/move/delegating constructor |
// PORTING: | dropped | red | `Bad` | enum | enum-def-rejected | Bad | enum-def-rejected | unsupported: enumerator 'fn' is a Rust keyword |
// PORTING: | stubbed | yellow | `use_box` | function | cxx-copy-ctor | use_box -> BoxI32 | rejected-type-cascade | unsupported: struct 'BoxI32' was rejected, so a type naming it cannot be imported |
// PORTING: | stubbed | yellow | `use_enum` | function | enum-def-rejected | use_enum -> Bad | rejected-type-cascade | unsupported: enum 'Bad' was rejected, so a type naming it cannot be imported |

// `attributed_via` names the rejected type the chain was resolved through,
// which is what makes the attribution auditable rather than trusted.
// JSON: "symbol": "use_box"
// JSON: "blocker": "rejected-type-cascade"
// JSON-NEXT: "diagnostic": "unsupported: struct 'BoxI32' was rejected, so a type naming it cannot be imported"
// JSON-NEXT: "root_blocker": "cxx-copy-ctor"
// JSON-NEXT: "attributed_via": "BoxI32"
// JSON-NEXT: "blame_chain": ["use_box", "BoxI32"]
// JSON: "symbol": "use_enum"
// JSON: "blocker": "rejected-type-cascade"
// JSON-NEXT: "diagnostic": "unsupported: enum 'Bad' was rejected, so a type naming it cannot be imported"
// JSON-NEXT: "root_blocker": "enum-def-rejected"
// JSON-NEXT: "attributed_via": "Bad"
// JSON-NEXT: "blame_chain": ["use_enum", "Bad"]
