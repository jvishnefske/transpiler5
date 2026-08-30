// FR-171 TRIPWIRE. This test exists to FAIL one day, loudly, on purpose.
//
// FR-171 was filed on the claim that a large population of "cause-
// asymmetric" function symbols -- symbols a shard calls but whose
// declaration the importer drops -- would turn into hard `unresolved
// external ... at link` obligations as soon as their declaration side
// became importable, and that a link-time stub cascade would therefore be
// needed to absorb them. Measurement falsified the premise: EVERY member of
// that population (83 of 83 in the systemd survey) carries a data `T **`
// parameter, and their declaration side is blocked TWICE.
//
//   1. `planCursorParamsFor` (lib/ImportC/ImportCPlanning.cpp) runs only
//      over `func->getBody()`, so a body-LESS declaration never reaches
//      cursor-parameter planning at all, and
//   2. it therefore falls through to lib/ImportC/ImportCTypes.cpp, which
//      rejects a data `T**` parameter UNCONDITIONALLY:
//      "unsupported: pointer-to-pointer parameter".
//
// That second, unconditional rejection is the ONLY thing keeping those 83
// symbols from becoming link obligations. A dropped declaration creates no
// obligation, so the call site degrades to an import-time
// `unimplemented!()` stub and the link is clean. That is what this file
// pins, end to end.
//
// If the pointer model ever lifts the `T **` parameter rejection for
// body-less DECLARATIONS without also lifting it for the DEFINITION side,
// this test flips: `mf` becomes a deferred external, the caller stops being
// a stub, and the link starts failing with `unresolved external 'mf'`. On
// that day FR-171 becomes live work and the stub-cascade question -- a
// measured NO-GO today, population zero -- has to be reopened. Do not
// "repair" this test by relaxing it; the failure IS the signal.
//
// The three links of the chain, each pinned separately:
//
// (1) Strict mode: the declaration's `char **` is a located hard rejection,
//     with the unconditional type-level wording (NOT the cursor-parameter
//     wording, which only a definition can earn), and no crate is written.
// RUN: not emitrust-cc --emit=crate %s -o %t.strict.crate 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT
// RUN: not ls %t.strict.crate
//
// (2) Recovery mode: the declaration is DROPPED under the `ptr-to-ptr`
//     ledger tag, and its caller is STUBBED because the callee is
//     unimported -- not deferred, not declared, not an obligation.
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/main.rs
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
//
// (3) The link: a shard built from this TU links CLEAN. No obligation was
//     ever recorded for `mf`, so there is nothing for the FR-58 merge to
//     resolve and nothing for a stub cascade to absorb.
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.o
// RUN: emitrust-cc --link %t.o --emit=rust -o %t.link.rs 2>%t.link.err
// RUN: FileCheck %s --check-prefix=LINKERR --input-file=%t.link.err \
// RUN:   --allow-empty
// RUN: FileCheck %s --check-prefix=LINKRS --input-file=%t.link.rs

struct MS { int a; int b; };

// The declaration side of the FR-171 population, reduced: a body-less
// function whose record parameter is fully importable and whose ONLY
// blocker is the data `char **`. No rejected type is cascading here --
// `struct MS` imports cleanly (see RUST below) -- so `ptr-to-ptr` is the
// root blocker, exactly as the survey re-measurement found once the
// cascade was removed.
// STRICT: :[[#@LINE+2]]:29: error: unsupported: pointer-to-pointer parameter
// WARN: :[[#@LINE+1]]:29: warning: unsupported: pointer-to-pointer parameter (recovered: item dropped)
int mf(struct MS *m, char **buf);

static char *tab[4];

// The caller. Its reference to `mf` is a plain call, so dropping `mf`
// leaves the call unresolvable at IMPORT time and the whole function
// becomes an `unimplemented!()` stub. This is the load-bearing step: an
// import-time stub is a local, contained loss, whereas a deferred external
// would have been a whole-program link obligation.
// WARN: :[[#@LINE+1]]:32: warning: unsupported: call to unimported function 'mf' (recovered: emitted an unimplemented!() stub with the mapped signature)
int use(struct MS *m) { return mf(m, tab); }

int main(void) { struct MS s[1] = {{1, 2}}; return use(s); }

// The recovery summary names the tag the survey counts on.
// WARN: recovered 3 rejected top-level items:
// WARN: dropped 'mf' [ptr-to-ptr] unsupported: pointer-to-pointer parameter
// WARN: dropped 'TU0_TAB' [other] unsupported: pointer type outside a parameter position
// WARN: stubbed 'use_' [other] unsupported: call to unimported function 'mf'
// WARN: blocker tabulation (recovered items by tag):
// WARN: ptr-to-ptr 1

// The record imports; `mf` leaves no trace at all in the crate -- no
// definition, no extern block, no declaration -- and the caller is the
// stub.
// RUST: struct Ms {
// RUST: unimplemented!("unsupported: call to unimported function 'mf'")
// RUST-NOT: fn mf(
// RUST-NOT: extern "C"

// JSON: "symbol": "mf"
// JSON-NEXT: "kind": "function"
// JSON-NEXT: "status": "dropped"
// JSON: "blocker": "ptr-to-ptr"
// JSON: "root_blocker": "ptr-to-ptr"

// THE tripwire assertion: the link produced no undefined-symbol error.
// LINKERR-NOT: unresolved external
// LINKERR-NOT: error:

// ...and the merged crate carries the stub rather than a dangling call.
// LINKRS: unimplemented!("unsupported: call to unimported function 'mf'")
// LINKRS-NOT: fn mf(
