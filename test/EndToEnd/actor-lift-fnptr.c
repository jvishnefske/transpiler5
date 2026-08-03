// REQUIRES: cargo
// FR-62 slice 4 (stage A) differential test, DEMOTION RULE 1: bump's
// address is taken (`g = bump`), so its actor cannot be lifted — a
// fn-pointer call site cannot thread the receiver. The pins: the demotion
// warning names the actor and the reason, the located remark points at the
// address-taking function, the program still BUILDS in today's
// thread-local form (demotion is not an error), and the byte-diff against
// the clang native is green because the module is untouched. (The plan
// also poison-merges through apply's indirect call; rule 1 is evaluated
// first, so the address-taking reason is the one reported.)
// RUN: emitrust-cc --actor-lift --emit=crate %s -o %t.crate --build 2> %t.err
// RUN: FileCheck %s < %t.err
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/actor_lift_fnptr > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// CHECK: warning: actor plan: demoted @stdout: function 'bump' has its address taken (a fn-pointer call site cannot thread the actor)
// CHECK: remark: actor plan: the address-taking use in 'c_main' keeps actor '@stdout' in the thread-local form

int printf(const char *, ...);
int counter;
int bump(void) { counter += 2; return counter; }
int apply(int (*f)(void)) { return f(); }
int main(void) {
  int (*g)(void) = bump;
  int r = apply(g);
  printf("r=%d c=%d\n", r, counter);
  return 0;
}
