// FR-126 (channel 2, the emission-wins guard): a
// `template-sibling-not-reached` row is a PER-TU fact -- it says one TU's
// walk never got there, not that the item is unportable -- so a symbol
// another TU emitted must read `ported`, whatever the first TU's walk did.
//
// The defect this exists to prevent was found adversarially during the
// FR-126 spike and is a hard LIE without the guard: in this two-TU build,
// TU1's walk dies on `pass<Nasty>` before reaching `pass<long>`, ledgering
// a not-reached row for `pass_i64` -- but TU2 demands `pass<long>` with
// nothing in the way, imports it, and `pass_i64` is EMITTED into the
// crate. A report that joins the TU1 row first would publish
// `dropped/template-sibling-not-reached` for a function sitting right
// there in src/, compiled and callable. Emission wins: a graph node whose
// ledger rows are ALL `template-sibling-not-reached` and whose symbol was
// emitted reads `ported`. (A REAL rejection row alongside an emitted
// symbol keeps its pre-existing stubbed/dropped reading -- the guard is
// deliberately narrow to the synthetic walk-abort tag.)
//
// RUN: emitrust-cc --emit=crate --incremental %s \
// RUN:   %S/Inputs/template-sibling-tu2.cpp -o %t.crate 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=PORTING --input-file=%t.crate/PORTING.md
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/lib.rs

#include "Inputs/template-sibling-common.h"

// --- TU1: instantiates pass<Nasty> FIRST, so this TU's walk never
// --- reaches its pass<long>.
int bad_first() {
  Nasty n;
  n.v = 2;
  return pass(n).v;
}
long unused1() { return pass(5L); }

// TU2 (Inputs/template-sibling-tu2.cpp) demands pass<long> with no bad
// sibling ahead of it, so the import emits pass_i64 there.

// The TU1 walk-abort row is really in the ledger -- the guard suppresses
// the JOIN for the emitted symbol, not the fact.
// WARN: dropped 'pass_i64' [template-sibling-not-reached] unsupported: specialization was not reached: sibling specialization 'pass_nasty' of the same template was rejected first

// The emitted function reads ported/green, not dropped.
// PORTING: | ported | green | `pass_i64` | function | - | - | - | - |

// JSON: "symbol": "pass_i64"
// JSON: "status": "ported"
// JSON: "blocker": ""
// JSON-NEXT: "diagnostic": ""
// JSON-NEXT: "root_blocker": ""
// JSON-NEXT: "attributed_via": ""
// JSON-NEXT: "blame_chain": []

// The symbol is genuinely in the crate.
// RUST: pub fn pass_i64(
