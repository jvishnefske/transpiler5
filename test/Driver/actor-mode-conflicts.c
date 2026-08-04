// FR-62 slice 5b/5c: pins the --actor-mode composition rules (the SLICE-5b
// SPIKE paragraph, verbatim UX contract; slice 5c promotes async from
// "immediate error" to a real mode under the SAME rules as threaded).
// (1) threaded and async REQUIRE the actor lift: E3's negative control
// proved a non-owned cluster on a thread is a silent miscompile, and an
// async task owns its state by the same argument, so --actor-lift=false
// conflicts immediately with either. (2) an explicit non-default
// --actor-mode on an emission mode with no lowered module is a usage
// error, mirroring --actor-lift's gate. (3) --actor-mode=threaded --link
// threads nothing and --actor-mode=async --link spawns nothing (FR-62
// F1a: the link lift is same-thread only; actor MODE under link is a
// recorded later stage) and each must SAY so — the link still succeeds,
// the F1a-3 default lifts the actor SAME-THREAD (struct CounterActor,
// no actor runtime anchor), and the async crate flavor's tokio manifest
// delta must NOT appear for a module with no async runtime.
// (4) the default (same-thread) composes with everything silently:
// byte-identical to the lift's output.
//
// RUN: not emitrust-cc --actor-mode=threaded --actor-lift=false --emit=rust %s -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CONFLICT
// CONFLICT: error: --actor-mode=threaded requires the actor lift (remove --actor-lift=false): ownership lift is a mandatory precondition of threaded mode
//
// RUN: not emitrust-cc --actor-mode=async --actor-lift=false --emit=rust %s -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=ACONFLICT
// ACONFLICT: error: --actor-mode=async requires the actor lift (remove --actor-lift=false): ownership lift is a mandatory precondition of async mode
//
// RUN: not emitrust-cc --actor-mode=threaded --emit=import %s -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=GATE
// RUN: not emitrust-cc --actor-mode=async --emit=import %s -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=GATE
// GATE: error: --actor-mode is only valid with --emit=mlir, --emit=rust or --emit=crate
//
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.o
// RUN: emitrust-cc --actor-mode=threaded --link %t.o --emit=rust -o %t.rs 2> %t.err
// RUN: FileCheck %s --check-prefix=LINKWARN < %t.err
// RUN: FileCheck %s --check-prefix=LINKRUST < %t.rs
// LINKWARN: warning: --actor-mode=threaded under --link threads nothing: actor mode under --link is a later stage (lifted actors stay same-thread)
// LINKRUST:     struct CounterActor
// LINKRUST-NOT: actor_rt
//
// RUN: emitrust-cc --actor-mode=async --link %t.o --emit=rust -o %t.async.rs 2> %t.async.err
// RUN: FileCheck %s --check-prefix=ALINKWARN < %t.async.err
// RUN: FileCheck %s --check-prefix=LINKRUST < %t.async.rs
// ALINKWARN: warning: --actor-mode=async under --link spawns nothing: actor mode under --link is a later stage (lifted actors stay same-thread)
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
