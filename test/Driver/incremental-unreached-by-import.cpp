// FR-115: an item the import NEVER VISITED says so, honestly.
//
// The defect this exists to prevent: a graph definition node with no ledger
// row and no emitted symbol used to read `status: missing` with blocker "",
// diagnostic "", root_blocker "" -- indistinguishable from a reporting bug.
// Measured corpus-wide those rows were the single largest silent bucket,
// and every one is an item only ever DEMANDED from code that was itself
// rejected first, so the import legitimately never reached it and has no
// diagnostic to report. Fabricating one would violate the
// located-diagnostics contract; instead the row carries the additive
// blocker tag `unreached-by-import` on the existing `missing` status (the
// 5-value status vocabulary the RealWorld ratchet parses is untouched), the
// diagnostic stays EMPTY (nothing was ever diagnosed), and FR-49 self-roots
// the item so the root ranking counts the unreached mass under its own
// honest bucket instead of scattering blanks.
//
// The construction (the fmt-header shape, minimized): the W2.15 template
// walk imports a class template's specializations in ONE loop that aborts
// on the first failure, so a bad EARLIER sibling instantiation
// (`Box<Nasty>`, poisoned by its field type) ends the walk before the good
// LATER one (`Box<long>`). The only other demand for `Box<long>` sits in
// the body of a planner-dropped variadic (`va_copy`), which the import
// never enters. Net: `BoxI64` is a graph definition node no import path
// ever visited -- no rejection, no emission, nothing to report but the
// non-visit itself.
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=PORTING --input-file=%t.crate/PORTING.md
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
//
// The tag is a REPORTING change only: the emitted Rust is byte-identical to
// what a plain --recover run of the same input produces.
// RUN: emitrust-cc --emit=crate --recover %s -o %t.recover.crate 2>/dev/null
// RUN: diff -r %t.crate/src %t.recover.crate/src

#include <stdarg.h>

// --- The poison: bodiless copy constructor, dropped [cxx-copy-ctor].
struct Nasty {
  int v;
  Nasty();
  Nasty(const Nasty &o);
};

// --- Importable if it were ever reached with a clean argument; nothing
// --- about Box<long> itself is rejected, which is exactly why no
// --- diagnostic exists for it.
template <typename T> struct Box {
  T v;
};

// --- Instantiates Box<Nasty> FIRST, so the template walk dies on it
// --- before reaching Box<long>.
int make_bad() {
  Box<Nasty> *p = 0;
  return p ? p->v.v : 0;
}

// --- The only other demander of Box<long>: dropped by the va planner, its
// --- body is never imported.
static int pick(int n, ...) {
  va_list ap, copy;
  va_start(ap, n);
  va_copy(copy, ap);
  va_end(copy);
  va_end(ap);
  Box<long> b;
  b.v = n;
  return (int)b.v;
}
int use_pick(void) { return pick(2, 3, 4); }

// --- In subset, so the crate is not empty.
int fine(int a) { return a + 1; }

// The unreached item is NOT in the ledger: it was never rejected, and an
// invented ledger row would be a fabricated diagnostic. (Its rejected
// SIBLING is: `BoxNasty` joins its node through the FR-115 graph-key
// ledger, and the pattern `Box` stays off-graph residue.)
// WARN: recovered
// WARN-NOT: 'BoxI64'

// The missing row carries the tag in the blocker column, its root column
// reads the same tag (self-rooted: no poison chain reaches an item nothing
// imported ever depended on), and its diagnostic cell is honestly empty.
// PORTING: | dropped | red | `BoxNasty` | record | copy-move-constructor | BoxNasty -> Nasty | rejected-type-cascade | unsupported: struct 'Nasty' was rejected, so a type naming it cannot be imported |
// PORTING: | missing | orange | `BoxI64` | record | unreached-by-import | BoxI64 | unreached-by-import | - |

// JSON: "missing": 1
// JSON: "symbol": "BoxI64"
// JSON: "status": "missing"
// JSON: "blocker": "unreached-by-import"
// JSON-NEXT: "diagnostic": ""
// JSON-NEXT: "root_blocker": "unreached-by-import"
// JSON-NEXT: "attributed_via": ""
// JSON-NEXT: "blame_chain": ["BoxI64"]
