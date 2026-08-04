// REQUIRES: cargo
// FR-62 F2 differential end-to-end test: owner-handle EXPORT for a library
// TU. The C below is a library (no `main`) whose mutable file-scope state
// plans as two actors; instead of rule-4 demotion the crate now exports
// each owner — a pub struct with PRIVATE fields, an associated
// `pub fn new()` carrying the C initializers, pub &mut-self arms (the
// internal-linkage `tu0_`-tagged arm stays a private method), and the
// cross client keeps its now-pub signature with one synthesized &mut
// parameter per actor (the intentional API arity change).
//
// The emitted crate is compiled as an rlib and driven by a hand-written
// consumer crate (Inputs/actor-lib-consumer.rs) that constructs both
// owners through new() and exercises arms, the internal-arm wrapper, and
// the cross function; the identical driver is provided in C by
// `#ifdef LIB_CRATE_MAIN`, compiled natively. The two must print the same
// bytes — the byte-diff is THE oracle that new() reproduces the C
// initializers and the threading discipline preserves C's effect order.
//
// (The rlib goes through --out-dir for its canonical `lib*.rlib` name:
// unlike lib-crate-external-caller.c, this test's %t does not happen to
// start with "lib", and rustc rejects an --extern path without the
// prefix.)
// RUN: emitrust-cc --emit=crate %s -o %t.crate
// RUN: rustc --edition=2021 --crate-type=rlib \
// RUN:   --crate-name=actor_lib_export %t.crate/src/lib.rs \
// RUN:   --out-dir %t.crate
// RUN: rustc --edition=2021 \
// RUN:   --extern actor_lib_export=%t.crate/libactor_lib_export.rlib \
// RUN:   %S/Inputs/actor-lib-consumer.rs -o %t.consumer
// RUN: %t.consumer > %t.rust.out
//
// RUN: clang -std=c11 -DLIB_CRATE_MAIN %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: diff %t.native.out %t.rust.out
//
// The fields really are private: a consumer touching one is an E0616
// (field privacy — not E0603, which is item privacy), the direct evidence
// that the exported owner can only be constructed through new() and
// mutated through the exported methods.
// RUN: echo 'fn main() { let mut a = actor_lib_export::CounterActor::new(); a.counter = 1; }' > %t.field.rs
// RUN: not rustc --edition=2021 --crate-name=fieldcheck \
// RUN:   --extern actor_lib_export=%t.crate/libactor_lib_export.rlib \
// RUN:   %t.field.rs -o %t.field 2>&1 | FileCheck %s --check-prefix=FIELD
// FIELD: E0616
//
// The internal-linkage arm really is a private method: calling it from
// the consumer is an E0624.
// RUN: echo 'fn main() { let mut a = actor_lib_export::CounterActor::new(); a.tu0_scale_step(1); }' > %t.method.rs
// RUN: not rustc --edition=2021 --crate-name=methodcheck \
// RUN:   --extern actor_lib_export=%t.crate/libactor_lib_export.rlib \
// RUN:   %t.method.rs -o %t.method 2>&1 | FileCheck %s --check-prefix=METHOD
// METHOD: E0624

// Actor 1: COUNTER, initialized — new() must reproduce the 5.
int counter = 5;

int bump(int by) {
  counter = counter + by;
  return counter;
}

int peek(void) { return counter; }

// Internal linkage: an arm of COUNTER that must stay a PRIVATE method.
static int scale_step(int v) {
  counter = counter * v;
  return counter;
}

// The pub surface over the private arm (an arm itself: its closure writes
// only COUNTER).
int bump_scaled(int v) { return scale_step(v) + 1; }

// Actor 2: TOTAL, zero-initialized — new() is bare Default.
int total;

int add_total(int x) {
  total = total + x;
  return total;
}

int get_total(void) { return total; }

// Cross client: direct footprint empty, closure reads both actors through
// their arms — it gains one &mut parameter per actor (sorted by actor
// name: counter_actor, total_actor).
int combined(void) { return peek() + get_total(); }

// The native oracle's driver, compiled only for the native leg: the same
// call sequence the Rust consumer makes, so the two libraries are compared
// on observable behavior.
#ifdef LIB_CRATE_MAIN
int printf(const char *, ...);

int main(void) {
  printf("bump=%d\n", bump(3));
  printf("scaled=%d\n", bump_scaled(2));
  printf("peek=%d\n", peek());
  printf("total=%d\n", add_total(10));
  printf("total=%d\n", add_total(-4));
  printf("combined=%d\n", combined());
  return 0;
}
#endif
