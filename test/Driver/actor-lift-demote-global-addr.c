// FR-62 slice 4 (stage B) regression, demotion rule 1b — found by the
// default flip (c-testsuite 00089): an owned GLOBAL whose address is taken
// escapes the planned actor surface, and the pass's IR-level veto cannot
// see the escape once the importer's returned-pointer rewrite has folded
// it away (here `expose` body-folds to a no-op and the driver reads STATE
// directly), so the AddressOfGlobal graph fact must demote at
// certification. Pins: the demote warning names the global, the located
// remark lands on the address-taking function, the global keeps its
// thread_local form, and no actor struct is synthesized. This program is
// 00089's shape minus the function pointers, so rule 1 (function address
// taken) stays silent and rule 1b alone carries the demotion.
// RUN: emitrust-cc --emit=rust %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --check-prefix=WARN < %t.err
// RUN: FileCheck %s --check-prefix=RUST --implicit-check-not=StateActor \
// RUN:   --implicit-check-not="&mut self" < %t.rs
//
// WARN:      warning: actor plan: demoted STATE: global 'STATE' has its address taken (the address escapes the actor surface)
// WARN:      remark: actor plan: the address-taking use in 'expose' keeps actor 'STATE' in the thread-local form
//
// RUST: thread_local!
// RUST: fn bump() -> i32

int printf(const char *, ...);

struct Counter {
  int value;
} state = {5};

struct Counter *expose(void) { return &state; }

int bump(void) {
  state.value += 2;
  return state.value;
}

int main(void) {
  int direct = expose()->value;
  int b = bump();
  printf("direct=%d b=%d\n", direct, b);
  return 0;
}
