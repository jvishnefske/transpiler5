// FR-126 (channel 2, function arm): when the W2.15 function-template walk
// aborts on a failed specialization, the never-visited LATER siblings get
// located `template-sibling-not-reached` rows attributed through the
// sibling that ended the walk -- and the failing specialization's OWN
// cause reaches the ledger under ITS graph key.
//
// The defect this exists to prevent, in two halves. (1) `pass_i64` below
// used to read `status: missing`, blocker `unreached-by-import`,
// self-rooted -- the fn-only #1 blocker bucket corpus-wide (30.0%), an
// attribution dead-end ranking nothing. The walk abort IS a located,
// mechanical fact, so it is ledgered as one. (2) Without an FR-115-shape
// per-specialization capture around the import of the FAILING sibling,
// its own cause is only recorded under the TEMPLATE's name (off-graph
// residue), so every not-reached row would dead-end at the synthetic tag
// instead of the real root -- measured at 248 corpus items during the
// FR-126 spike. The capture gives `pass_nasty` its own graph-key row, and
// `pass_i64`'s chain resolves through it to `Nasty`'s copy constructor.
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=PORTING --input-file=%t.crate/PORTING.md
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
//
// The rows are a REPORTING change only: the emitted Rust is byte-identical
// to what a plain --recover run of the same input produces.
// RUN: emitrust-cc --emit=crate --recover %s -o %t.recover.crate 2>/dev/null
// RUN: diff -r %t.crate/src %t.recover.crate/src
//
// The capture-replay around the failing specialization preserves strict
// mode: one located error, no duplicate from the re-emission.
// RUN: not emitrust-cc --emit=crate %s -o %t.strict 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT

// --- The poison: bodiless copy constructor, dropped [cxx-copy-ctor].
struct Nasty {
  int v;
  Nasty();
  Nasty(const Nasty &o);
};

// --- One template, two specializations. The walk visits them in
// --- instantiation order and aborts on the first failure.
template <typename T> T pass(T x) { return x; }

// --- Instantiates pass<Nasty> FIRST (rejected: Nasty in the signature),
// --- ending the walk before pass<long>.
int bad_first() {
  Nasty n;
  n.v = 2;
  return pass(n).v;
}

// --- The later sibling's only demander; its body still stubs (the call
// --- targets an unimported function), which is precisely why the sibling
// --- row must exist for the attribution to survive.
long good_later() { return pass(7L); }

// --- In subset, so the crate is not empty.
int fine(int a) { return a + 1; }

// Both new rows surface in the recovery summary: the failing spec under
// its own graph key with its real (cascade) cause, and the never-visited
// sibling against it.
// WARN: recovered
// WARN: dropped 'pass_nasty' [rejected-type-cascade] unsupported: struct 'Nasty' was rejected, so a type naming it cannot be imported
// WARN: dropped 'pass_i64' [template-sibling-not-reached] unsupported: specialization was not reached: sibling specialization 'pass_nasty' of the same template was rejected first

// The chain walks not-reached -> failing sibling -> its rejected type.
// PORTING: ## Project items
// PORTING: | dropped | red | `Nasty` | record | copy-move-constructor | Nasty | cxx-copy-ctor | unsupported: copy/move/delegating constructor |
// PORTING: | dropped | red | `pass_nasty` | function | copy-move-constructor | pass_nasty -> Nasty | rejected-type-cascade | unsupported: struct 'Nasty' was rejected, so a type naming it cannot be imported |
// PORTING: | dropped | red | `pass_i64` | function | copy-move-constructor | pass_i64 -> pass_nasty -> Nasty | template-sibling-not-reached | unsupported: specialization was not reached: sibling specialization 'pass_nasty' of the same template was rejected first |

// JSON: "symbol": "pass_i64"
// JSON: "status": "dropped"
// JSON: "blocker": "template-sibling-not-reached"
// JSON-NEXT: "diagnostic": "unsupported: specialization was not reached: sibling specialization 'pass_nasty' of the same template was rejected first"
// JSON-NEXT: "root_blocker": "copy-move-constructor"
// JSON-NEXT: "attributed_via": "pass_nasty"
// JSON-NEXT: "blame_chain": ["pass_i64", "pass_nasty", "Nasty"]

// STRICT: error: unsupported: copy/move/delegating constructor
// STRICT-NOT: error: unsupported: copy/move/delegating constructor
