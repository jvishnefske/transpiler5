// FR-62 slice 5b: pins the --actor-mode composition rules (the SLICE-5b
// SPIKE paragraph, verbatim UX contract). (1) async is defined in the
// attribute space but NOT yet emitted — requesting it is an immediate
// error, never a silent fallback to the threaded flavor. (2) threaded
// REQUIRES the actor lift: E3's negative control proved a non-owned
// cluster on a thread is a silent miscompile, so --actor-lift=false
// conflicts immediately. (3) an explicit non-default --actor-mode on an
// emission mode with no lowered module is a usage error, mirroring
// --actor-lift's gate. (4) --actor-mode=threaded --link threads nothing
// (every actor is rule-5 demoted) and must SAY so — the link still
// succeeds and keeps the thread_local form. (5) the default (same-thread)
// composes with everything silently: byte-identical to the lift's output.
//
// RUN: not emitrust-cc --actor-mode=async --emit=rust %s -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=ASYNC
// ASYNC: error: --actor-mode=async is not yet emitted (the async crate flavor is a recorded later slice)
//
// RUN: not emitrust-cc --actor-mode=threaded --actor-lift=false --emit=rust %s -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CONFLICT
// CONFLICT: error: --actor-mode=threaded requires the actor lift (remove --actor-lift=false): ownership lift is a mandatory precondition of threaded mode
//
// RUN: not emitrust-cc --actor-mode=threaded --emit=import %s -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=GATE
// GATE: error: --actor-mode is only valid with --emit=mlir, --emit=rust or --emit=crate
//
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.o
// RUN: emitrust-cc --actor-mode=threaded --link %t.o --emit=rust -o %t.rs 2> %t.err
// RUN: FileCheck %s --check-prefix=LINKWARN < %t.err
// RUN: FileCheck %s --check-prefix=LINKRUST < %t.rs
// LINKWARN: warning: --actor-mode=threaded under --link threads nothing: every actor is demoted (rule 5)
// LINKRUST:     thread_local!
// LINKRUST-NOT: actor_rt
//
// RUN: emitrust-cc --actor-mode=same-thread --emit=rust %s -o %t.same.rs
// RUN: emitrust-cc --emit=rust %s -o %t.default.rs
// RUN: diff %t.same.rs %t.default.rs

int counter;

int bump(void) {
  counter += 1;
  return counter;
}

int main(void) { return bump(); }
