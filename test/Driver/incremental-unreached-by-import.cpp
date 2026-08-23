// FR-115, then FR-126 (channel 2): an item the import never visited
// because its TEMPLATE WALK died on an earlier sibling now says WHY, with
// a located row attributed through that sibling.
//
// The FR-115 frontier this test used to pin: a graph definition node no
// import path ever visited read `status: missing` with the additive
// blocker tag `unreached-by-import`, an honestly EMPTY diagnostic, and a
// self-rooted chain -- on the doctrine that the import never diagnosed
// anything for it and an invented ledger row would be a fabricated
// diagnostic. Measured on the 110-unit C++ corpus that bucket was the
// fn-only #1 blocker at 30.0% (448 of 1494), which is an attribution
// dead-end: "we never looked" ranks nothing. FR-126 moves the pin
// FORWARD for the template-walk shape: the walk's abort on a failed
// sibling instantiation is itself a located, mechanical FACT, so the
// importer ledgers each never-visited later sibling against the sibling
// that aborted the walk (`template-sibling-not-reached`), and FR-49's
// chain resolves through it to the real root. The row is not fabricated:
// its location is the specialization's own point of instantiation and its
// message states exactly what happened. After the flip the corpus fn-only
// unreached share is 1.4% (21 of 1505); what remains of
// `unreached-by-import` (genuinely undemanded items outside the two
// template walk loops) keeps the FR-115 reading.
//
// The construction (the fmt-header shape, minimized): the W2.15 template
// walk imports a class template's specializations in ONE loop that aborts
// on the first failure, so a bad EARLIER sibling instantiation
// (`Box<Nasty>`, poisoned by its field type) ends the walk before the good
// LATER one (`Box<long>`). The only other demand for `Box<long>` sits in
// the body of a planner-dropped variadic (`va_copy`), which the import
// never enters. Net: `BoxI64` is a graph definition node no import path
// ever visited -- and the ONLY thing known about it is the walk abort the
// new row records.
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
// --- about Box<long> itself is rejected. The sibling row does not claim
// --- otherwise: it reports the WALK ABORT, not a defect of Box<long>.
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

// The never-visited sibling is IN the ledger now, named against the
// sibling whose failure ended the walk. (Its rejected sibling `BoxNasty`
// still joins its node through the FR-115 graph-key ledger, and the
// pattern `Box` stays off-graph residue.)
// WARN: recovered
// WARN: dropped 'BoxNasty' [rejected-type-cascade] unsupported: struct 'Nasty' was rejected, so a type naming it cannot be imported
// WARN: dropped 'BoxI64' [template-sibling-not-reached] unsupported: specialization was not reached: sibling specialization 'BoxNasty' of the same template was rejected first

// The row is dropped-with-a-cause instead of missing-with-a-blank, and its
// chain walks through the failed sibling to the sibling's own root.
// PORTING: | dropped | red | `BoxNasty` | record | copy-move-constructor | BoxNasty -> Nasty | rejected-type-cascade | unsupported: struct 'Nasty' was rejected, so a type naming it cannot be imported |
// PORTING: | dropped | red | `BoxI64` | record | copy-move-constructor | BoxI64 -> BoxNasty -> Nasty | template-sibling-not-reached | unsupported: specialization was not reached: sibling specialization 'BoxNasty' of the same template was rejected first |

// JSON: "missing": 0
// JSON: "symbol": "BoxI64"
// JSON: "status": "dropped"
// JSON: "blocker": "template-sibling-not-reached"
// JSON-NEXT: "diagnostic": "unsupported: specialization was not reached: sibling specialization 'BoxNasty' of the same template was rejected first"
// JSON-NEXT: "root_blocker": "copy-move-constructor"
// JSON-NEXT: "attributed_via": "BoxNasty"
// JSON-NEXT: "blame_chain": ["BoxI64", "BoxNasty", "Nasty"]
