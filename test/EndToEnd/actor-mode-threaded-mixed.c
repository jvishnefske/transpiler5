// REQUIRES: cargo
// FR-62 slice 5b differential test, the MIXED shape: one eligible actor
// threads while two ineligible ones stay lifted same-thread, in ONE
// module, and the whole program still byte-diffs against the clang
// native. CounterActor (independent cluster, no cross clients) gets the
// full mailbox runtime; LeftActor and RightActor are read through the
// call-mediated cross client `observe`, which after the lift holds one
// &mut parameter per actor — a borrow that cannot cross a thread
// boundary (the E3 landmine rule), so BOTH veto with the pinned located
// warning and keep the slice-4 struct shape (structs still present, no
// handle for either). Demote-from-threading is not demote-from-lifting:
// no thread_local survives for any of the three.
// RUN: emitrust-cc --actor-mode=threaded --emit=crate %s -o %t.crate --build 2> %t.err
// RUN: FileCheck %s --check-prefix=WARN < %t.err
// RUN: grep "type CounterActorHandle = actor_rt::Handle<CounterActorMsg>;" %t.crate/src/main.rs
// RUN: not grep "LeftActorHandle" %t.crate/src/main.rs
// RUN: not grep "RightActorHandle" %t.crate/src/main.rs
// RUN: grep "struct LeftActor" %t.crate/src/main.rs
// RUN: grep "struct RightActor" %t.crate/src/main.rs
// RUN: not grep "thread_local" %t.crate/src/main.rs
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/actor_mode_threaded_mixed > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// WARN: warning: actor plan: LeftActor stays same-thread: function 'observe' holds a &mut reference across the thread boundary
// WARN: warning: actor plan: RightActor stays same-thread: function 'observe' holds a &mut reference across the thread boundary

int printf(const char *, ...);

int counter;
int left;
int right;

int bump(void) {
  counter += 5;
  return counter;
}

int bump_left(void)  { left += 1; return left; }
int bump_right(void) { right += 3; return right; }
int get_left(void)   { return left; }
int get_right(void)  { return right; }

/* closure reads both LEFT and RIGHT through calls: a cross client */
int observe(int k) { return get_left() + get_right() + k; }

int main(void) {
  printf("b=%d\n", bump());
  int a = bump_left() + bump_right();
  printf("a=%d\n", a);
  printf("obs=%d\n", observe(2));
  printf("b2=%d\n", bump());
  return 0;
}
