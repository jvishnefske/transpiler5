// REQUIRES: cargo
// FR-130 increment 2 (dead-binding elimination), PURITY-GATE leg. Dropping a
// never-read binding together with every assignment that writes it is only
// sound when those assignments carry effect-free right-hand sides; deleting an
// assignment whose RHS is a CALL would silently delete the call. That is the
// one failure direction rustc cannot catch: `deny(unused_variables)` and
// `deny(unused_assignments)` are structurally blind here, because the emitter
// already `_`-prefixes a never-read binding's name. So this test pins the
// purity gate two ways at once, on the SCF-lowering shape that produces
// never-read bindings in bulk (Duff's device: an irreducible switch whose
// SSA destruction materializes a default-initialized `let mut` per carried
// value, written in every arm):
//   1. `side_total` is an observable accumulator bumped once per copy step;
//      if any dropped assignment ever took a `bump()` call with it, the
//      printed total changes and the byte-diff against the clang-built
//      native binary fails LOUDLY.
//   2. FileCheck on the emitted Rust pins that every `bump` call site
//      survives as its own statement -- the actual purity invariant, which
//      stdout equality alone would only sample.
// Native output is `5 10`.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/dead_binding_impure_rhs > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s

// Eight call sites in the loop body (one per Duff residue arm) plus none
// elsewhere: each renders its own `let _vNN: i32 = self.tu0_bump(..);`
// statement, because an impure producer is never folded into an operand
// position. The binding is `_`-prefixed (the call's value is dead) yet the
// CALL itself must stay -- that is exactly the pair the purity gate protects.
// CHECK-COUNT-8: self.tu0_bump(
// CHECK-NOT: self.tu0_bump(

int printf(const char *, ...);
int side_total = 0;
static int bump(int x) { side_total += x; return side_total; }

int duff_call(int count) {
  int i = 0, n = (count + 7) / 8;
  int last = 0;
  switch (count % 8) {
  case 0: do { last = bump(i); i++;
  case 7:      last = bump(i); i++;
  case 6:      last = bump(i); i++;
  case 5:      last = bump(i); i++;
  case 4:      last = bump(i); i++;
  case 3:      last = bump(i); i++;
  case 2:      last = bump(i); i++;
  case 1:      last = bump(i); i++;
        } while (--n > 0);
  }
  return i;
}
int main(void) { printf("%d %d\n", duff_call(5), side_total); return 0; }
