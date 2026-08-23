// FR-53: a Pass-A PLANNER rejection is recovered per declaration, exactly as
// an `importFunction` rejection is (FR-42).
//
// The planners run BEFORE the declaration walk and used to return `failure()`
// straight out of `importTranslationUnit`, so this whole class of rejection sat
// structurally outside recovery: one `char **` parameter outside the
// string-cursor shape killed the translation unit and `--incremental` wrote no
// crate at all -- no `src/`, no `PORTING.md`, no `emitrust-progress.json` --
// even though every other item was perfectly translatable. On real third-party
// C that was the dominant failure mode, not a corner case.
//
// The rejection is attributable: its location is the offending parameter of ONE
// definition, so that definition is credited with it, dropped or stubbed by the
// walk, and everything else ports.
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/main.rs
// RUN: FileCheck %s --check-prefix=PORTING --input-file=%t.crate/PORTING.md
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
//
// Recovery OFF is unchanged: the same diagnostic, as an ERROR, a non-zero
// exit, and no output directory. That is the property the whole change is
// guarded on -- `recoverFromRejections` is the only thing that switches the
// planners' behavior.
// RUN: not emitrust-cc --emit=crate %s -o %t.strict.crate 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT
// RUN: not ls %t.strict.crate

#include <stddef.h>

static int helper(int x) { return x + 1; }

// The `unsigned char **` parameter is written THROUGH (`(*out)[0] = ...`),
// which is the one shape the string-cursor plan cannot take. The recovered
// warning must point HERE -- at the offending expression, the location the
// non-recovering error carried -- because that precision is what makes the
// rejection attributable to one declaration in the first place.
static int consume(unsigned char **out, size_t n) {
  // WARN: :[[#@LINE+1]]:3: warning: unsupported: pointer-to-pointer parameter escapes the cursor-parameter shape (recovered: item dropped)
  (*out)[0] = (unsigned char)n;
  return 0;
}

int main(void) { return helper(1); }

// The driver exits 0 and summarizes what it dropped.
// WARN: recovered 1 rejected top-level item:
// WARN: dropped 'tu0_consume' [ptr-to-ptr-shape-escape] unsupported: pointer-to-pointer parameter escapes the cursor-parameter shape

// Both translatable items reach the crate; the rejected one leaves no trace.
// RUST: fn tu0_helper(x: i32) -> i32 {
// RUST: fn c_main() -> i32 {
// RUST:     tu0_helper(1i32)
// RUST-NOT: consume

// The two translatable items are counted as ported and the report names the
// construct that cost the third, so the crate ships with an honest account of
// itself instead of not shipping at all.
//
// FR-115 closed the FR-44 join gap this test used to pin: the recovery
// ledger now keys a dropped item by the item graph's own vocabulary
// (`graphItemSymbol`, the same `cFunctionSymbolName(func, tuTag)` the graph
// uses), so `consume` joins its `tu0_consume` node as a `dropped` graph row
// with its blocker and diagnostic, and the off-graph table is empty.
// PORTING: **2 of 3 items ported (66.6%).**
// PORTING: | ported | 2 |
// PORTING-NEXT: | stubbed | 0 |
// PORTING: | root blocker | items |
// PORTING: | ptr-to-ptr-shape-escape | 1 |
// PORTING: | dropped | red | `tu0_consume` | function | ptr-to-ptr-shape-escape | tu0_consume | ptr-to-ptr-shape-escape | unsupported: pointer-to-pointer parameter escapes the cursor-parameter shape |
// PORTING-NOT: ## Rejected items outside the item graph

// JSON: "graph_items": 3
// JSON-NEXT: "ported": 2
// JSON-NEXT: "stubbed": 0
// JSON: { "tag": "ptr-to-ptr-shape-escape", "count": 1 }
// JSON: "symbol": "tu0_consume"
// JSON: "status": "dropped"
// JSON: "off_graph_items": []

// STRICT: error: unsupported: pointer-to-pointer parameter escapes the cursor-parameter shape
