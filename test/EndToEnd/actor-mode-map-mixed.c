// REQUIRES: cargo
// FR-62 F3 differential test, the PER-ACTOR-MAP mixed shape: two actors
// that are BOTH thread-eligible, but the --actor-mode-map opts one of
// them out, in ONE module, and the whole program still byte-diffs
// against the clang native. Under the global --actor-mode=threaded,
// CounterActor keeps the full mailbox runtime while TotalActor's
// `same-thread` map entry (shipped in Inputs/, the --actor-map tests'
// convention) EXCLUDES it from the anchor list — not a veto, a choice:
// unlike actor-mode-threaded-mixed.c's cross-client warning, this run
// must warn about NOTHING. Opting out of threading is not opting out of
// the lift: TotalActor keeps the slice-4 struct shape and no
// thread_local survives for either actor.
// RUN: emitrust-cc --actor-mode=threaded --actor-mode-map %S/Inputs/actor-mode-map-mixed.map --emit=crate %s -o %t.crate --build 2> %t.err
// RUN: not grep "warning" %t.err
// RUN: grep "type CounterActorHandle = actor_rt::Handle<CounterActorMsg>;" %t.crate/src/main.rs
// RUN: not grep "TotalActorHandle" %t.crate/src/main.rs
// RUN: grep "struct TotalActor" %t.crate/src/main.rs
// RUN: not grep "thread_local" %t.crate/src/main.rs
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/actor_mode_map_mixed > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int counter;
int total;

int bump(void) {
  counter += 5;
  return counter;
}

int add(int x) {
  total += x;
  return total;
}

int get_total(void) { return total; }

int main(void) {
  printf("b=%d\n", bump());
  int a = add(3);
  printf("a=%d\n", a);
  printf("t=%d\n", add(4));
  printf("b2=%d\n", bump());
  printf("g=%d\n", get_total());
  return 0;
}
