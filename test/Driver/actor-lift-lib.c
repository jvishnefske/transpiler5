// FR-62 F2 (owner-handle export), the driver-visible surface for a LIBRARY
// TU: pins the invariant that demotion rule 4 no longer demotes a library
// unit's certified actors — they EXPORT as owner handles (pub struct with
// private fields, an associated `pub fn new()` carrying the C
// initializers, pub &mut-self arms) with a per-actor `note:` on stderr —
// while every OTHER demotion rule still demotes with the standard warning:
// here rule 1 (an arm's address is taken) and the F2 FR-52 veto (an arm
// generic over the external-requirements trait must not export, because a
// method call cannot carry the type parameter — the demoted thread-local
// form is the proven generic-compatible shape). The cross client keeps its
// now-pub signature with the synthesized &mut parameter for the EXPORTED
// actor only; the demoted actors keep today's thread_local form.
// RUN: emitrust-cc --emit=rust %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --check-prefix=NOTE < %t.err
// RUN: FileCheck %s --check-prefix=RUST < %t.rs

// The exported actor: external-linkage arms over one initialized global.
int counter = 5;

int bump(int by) {
  counter = counter + by;
  return counter;
}

int peek(void) { return counter; }

// FR-52 veto: host_scale is declared, never defined — a library
// requirement — so add_total is generic over the Externals trait and its
// actor must demote rather than export an uncallable method.
int host_scale(int v);

int total;

int add_total(int x) {
  total = total + host_scale(x);
  return total;
}

// Rule 1 still demotes: poke's address is taken by hook(), so the FLAGS
// actor keeps the thread-local form (a fn-pointer call site cannot thread
// the receiver).
int flags;

int poke(void) {
  flags = flags + 1;
  return flags;
}

int get_flags(void) { return flags; }

int (*hook(void))(void) { return poke; }

// Cross client: direct footprint empty, closure reads COUNTER (exported)
// and FLAGS (demoted) — it gains the &mut parameter only for the exported
// actor.
int summary(void) { return peek() + get_flags(); }

// One note per exported actor; one warning per demoted actor, standard
// spelling.
// NOTE-DAG: note: actor plan: exported COUNTER: owner handle 'CounterActor' (construct with CounterActor::new())
// NOTE-DAG: warning: actor plan: demoted FLAGS: function 'poke' has its address taken (a fn-pointer call site cannot thread the actor)
// NOTE-DAG: warning: actor plan: demoted TOTAL: arm 'add_total' is generic over the external-requirements trait (an exported method call cannot carry the type parameter)

// The exported owner: pub struct, PRIVATE fields, pub new() carrying the
// C initializer, pub &mut-self arms.
// RUST:      pub struct CounterActor {
// RUST-NEXT:     counter: i32,
// RUST-NEXT: }
// RUST:      impl CounterActor {
// RUST-NEXT:     pub fn new() -> CounterActor {
// RUST-NEXT:         let owner: CounterActor = CounterActor { counter: 5i32, };
// RUST-NEXT:         owner
// RUST-NEXT:     }
// RUST:          pub fn bump(&mut self, by: i32) -> i32 {
// RUST:          pub fn peek(&mut self) -> i32 {

// The demoted actors keep the thread-local form; the generic arm keeps its
// module-level generic signature.
// RUST:      pub fn add_total<E: Externals>(x: i32) -> i32 {
// RUST:      pub fn poke() -> i32 {

// The cross client: pub, one &mut parameter for the exported actor only
// (FLAGS demoted), the arm call a method call on it.
// RUST:      pub fn summary(counter_actor: &mut CounterActor) -> i32 {
// RUST-NEXT:     let v0: i32 = counter_actor.peek();
// RUST-NEXT:     let v1: i32 = get_flags();
// RUST-NEXT:     v0 + v1
