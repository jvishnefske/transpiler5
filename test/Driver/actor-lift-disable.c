// FR-62 slice 4 (stage B): the actor lift is DEFAULT ON, and
// `--actor-lift=false` is the disable path (LLVM bool-flag convention; the
// driver has no other negatable flag). This pins that disabling restores
// the pre-flip form exactly on a program the certification would otherwise
// lift: the single-global cluster stays in today's thread_local accessor
// form and no actor struct, impl block or `&mut self` method is
// synthesized (--implicit-check-not on both actor spellings).
// RUN: emitrust-cc --actor-lift=false --emit=rust %s -o - \
// RUN:   | FileCheck %s --implicit-check-not=CounterActor \
// RUN:       --implicit-check-not="&mut self"

int counter;

int bump(void) {
  counter += 1;
  return counter;
}

int main(void) { return bump(); }

// CHECK: thread_local!
// CHECK: fn bump() -> i32
