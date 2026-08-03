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
  // WARN: :[[#@LINE+1]]:3: warning: unsupported: pointer-to-pointer parameter escapes the string-cursor shape (recovered: item dropped)
  (*out)[0] = (unsigned char)n;
  return 0;
}

int main(void) { return helper(1); }

// The driver exits 0 and summarizes what it dropped.
// WARN: recovered 1 rejected top-level item:
// WARN: dropped 'consume' [ptr-to-ptr] unsupported: pointer-to-pointer parameter escapes the string-cursor shape

// Both translatable items reach the crate; the rejected one leaves no trace.
// RUST: fn tu0_helper(v0: i32) -> i32 {
// RUST: fn c_main() -> i32 {
// RUST:     tu0_helper(1i32)
// RUST-NOT: consume

// The two translatable items are counted as ported and the report names the
// construct that cost the third, so the crate ships with an honest account of
// itself instead of not shipping at all.
//
// `consume` lands in the OFF-GRAPH table rather than as a `dropped` graph item.
// That is a pre-existing FR-44 join gap, not an FR-53 behavior: the ledger
// records a dropped item under its C spelling (`declLedgerName`) while the
// item graph keys an internal-linkage function under its TU-tagged emitted
// name (`tu0_consume`), so the two never join and the graph row stays
// `missing`. It reproduces identically for a function dropped by an ordinary
// `importFunction` rejection (a `static` returning `_Complex double`), on a
// build with no FR-53 change in it at all. Pinned here as-is so that whoever
// closes the join gap sees this test, and so this test is not silently
// asserting that the gap is correct.
// PORTING: **2 of 3 items ported (66.6%).**
// PORTING: | ported | 2 |
// PORTING-NEXT: | stubbed | 0 |
// PORTING: | root blocker | items |
// PORTING: | ptr-to-ptr | 1 |
// PORTING: ## Rejected items outside the item graph
// PORTING: | dropped | red | `consume` | - | ptr-to-ptr | consume | ptr-to-ptr | unsupported: pointer-to-pointer parameter escapes the string-cursor shape |

// JSON: "graph_items": 3
// JSON-NEXT: "ported": 2
// JSON-NEXT: "stubbed": 0
// JSON: { "tag": "ptr-to-ptr", "count": 1 }
// JSON: "off_graph_items": [
// JSON-NEXT: {
// JSON-NEXT: "symbol": "consume"
// JSON: "status": "dropped"

// STRICT: error: unsupported: pointer-to-pointer parameter escapes the string-cursor shape
