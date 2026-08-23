// FR-125 x FR-115: the CamelCase-namespace snake_case fold reaches the
// FR-40 item graph and the progress report through the SAME naming
// primitive as the emitted crate, so the join never diverges.
//
// The hazard this pins against: emitted symbol names are the
// CSymbolNaming byte-identity contract shared between the importer,
// `ItemGraphBuilder`, and the recovery ledger (FR-115 fixed exactly that
// join). FR-125 changes what `namespacePrefix` spells (`namespace Game`
// -> `ns_game_`), so a fix applied anywhere but the single primitive
// would re-open the divergence: the graph would key `ns_Game_*` rows the
// crate spells `ns_game_*`, the rejection ledger would miss its node,
// and the row would read `missing` with a blank blocker -- the measured
// pre-FR-115 failure mode. Pinned here: a ported function, a dropped
// record, and its cascade-dropped user, all inside `namespace Game`,
// agree on the FOLDED graph keys across the recovery summary, the
// PORTING table, the progress JSON, and the crate source itself.
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=PORTING --input-file=%t.crate/PORTING.md
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
// RUN: FileCheck %s --check-prefix=CRATE --input-file=%t.crate/src/lib.rs

namespace Game {

// --- Rejected on its MOVE constructor: graph key `NsGameGadget` (the
// --- UpperCamel fold erased the segment's case before FR-125 -- this
// --- spelling is fold-invariant).
struct Gadget {
  int v;
  Gadget() : v(0) {}
  Gadget(Gadget &&other) : v(other.v) {}
};

// --- Cascade-dropped user: graph key `ns_game_bad` -- the FOLDED
// --- prefix, the spelling FR-125 changed.
int bad(Gadget g) { return g.v; }

// --- In subset: graph key `ns_game_score`, and the crate must spell
// --- the definition with the SAME folded name the JSON reports.
int score(int s) { return s + 1; }

} // namespace Game

// The recovery ledger names both rejected items by their FOLDED graph
// keys -- neither row may surface a verbatim `ns_Game_*` spelling.
// WARN: recovered 2 rejected top-level items:
// WARN-NEXT: dropped 'NsGameGadget' [cxx-copy-ctor] unsupported: copy/move/delegating constructor
// WARN-NEXT: dropped 'ns_game_bad' [rejected-type-cascade] unsupported: struct 'NsGameGadget' was rejected, so a type naming it cannot be imported

// PORTING: ## Project items
// PORTING: | dropped | red | `NsGameGadget` | record | copy-move-constructor | NsGameGadget | cxx-copy-ctor | unsupported: copy/move/delegating constructor |
// PORTING-NEXT: | dropped | red | `ns_game_bad` | function | copy-move-constructor | ns_game_bad -> NsGameGadget | rejected-type-cascade | unsupported: struct 'NsGameGadget' was rejected, so a type naming it cannot be imported |
// PORTING-NEXT: | ported | green | `ns_game_score` | function |

// Every row is a graph row (nothing `missing`, nothing off-graph), and
// the keys are the folded spellings.
// JSON: "graph_items": 3
// JSON-NEXT: "ported": 1
// JSON-NEXT: "stubbed": 0
// JSON-NEXT: "dropped": 2
// JSON-NEXT: "missing": 0
// JSON-NEXT: "declared": 0
// JSON-NEXT: "off_graph_rejected": 0
// JSON: "symbol": "NsGameGadget"
// JSON: "status": "dropped"
// JSON: "blocker": "cxx-copy-ctor"
// JSON: "symbol": "ns_game_bad"
// JSON: "status": "dropped"
// JSON: "blocker": "rejected-type-cascade"
// JSON-NEXT: "diagnostic": "unsupported: struct 'NsGameGadget' was rejected, so a type naming it cannot be imported"
// JSON: "symbol": "ns_game_score"
// JSON: "status": "ported"

// The crate spells the surviving definition with the JSON's own key --
// the byte-for-byte join FR-115 exists to protect.
// CRATE: pub fn ns_game_score(s: i32) -> i32 {
