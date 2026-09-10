// FR-106 byte-identity guard: a function with no dead store gains NO
// attribute, and the crate header gains nothing either.
//
// FR-106 adds `#[allow(unused_assignments)]` to the individual `fn` that
// provably holds a store rustc's `unused_assignments` would flag. It must
// add it NOWHERE else: measured over the whole test corpus (890 inputs)
// and 473 external crates, the change moves exactly 4 lines in 4 crates
// and not one byte anywhere else. An attribute that leaks onto a clean
// function is a behavior change (it deletes the FR-53 deny's tripwire on
// that function), and an attribute in the crate root is direction (b),
// which was rejected outright.
//
// The whole emitted crate is pinned line-by-line so a stray attribute
// anywhere -- crate header, `impl`, method, free fn -- shifts a line and
// fails. The shapes covered: a loop-carried accumulator that is read every
// iteration (`sum_to`), an ELIDED dead store whose `s = 0` never reaches
// the output at all (`elided`, so a detector that counted non-rendered
// stores would mark it), and an actor METHOD inside an `impl` (`tally`,
// the nested-item insertion site).
//
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s
// CHECK-NOT: allow(unused_assignments)

int printf(const char *, ...);
static int total;

int sum_to(int n) {
  int s = 0, i;
  for (i = 0; i < n; i++) s += i;
  return s;
}

int elided(int n) {
  int s = 0;
  s = n * 3;
  return s;
}

void tally(int n) {
  int i;
  for (i = 0; i < n; i++) total += i;
}

int main(void) {
  tally(5);
  printf("%d %d %d\n", sum_to(4), elided(5), total);
  return 0;
}

// FR-220 did to `dead_code` exactly what FR-106 did to `unused_assignments`,
// and for the same stated reason: a crate-root allow deletes the tripwire on
// every item at once. The `#![allow(dead_code)]` header and its blank line are
// gone; the actor struct and the INHERENT impl each carry their own targeted
// `#[allow(dead_code)]`, and the three free functions carry none -- which is
// the point, because a dead emitted `fn` is the one shape that would signal an
// emitter defect. `unused_assignments` is untouched: the CHECK-NOT sweep that
// opens this file and the one that closes it both still hold.
// CHECK:      #[allow(dead_code)]
// CHECK-NEXT: #[derive(Clone, Copy, Default)]
// CHECK-NEXT: struct Tu0TotalActor {
// CHECK-NEXT:     tu0_total: i32,
// CHECK-NEXT: }
// CHECK-NEXT: #[allow(dead_code)]
// CHECK-NEXT: impl Tu0TotalActor {
// CHECK-NEXT:     fn tally(&mut self, n: i32) {
// CHECK-NEXT:         for i in 0i32..n {
// CHECK-NEXT:             self.tu0_total += i;
// CHECK-NEXT:         }
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK-NEXT: fn sum_to(n: i32) -> i32 {
// CHECK-NEXT:     let mut s: i32 = 0;
// CHECK-NEXT:     for i in 0i32..n {
// CHECK-NEXT:         s += i;
// CHECK-NEXT:     }
// CHECK-NEXT:     s
// CHECK-NEXT: }
// CHECK-NEXT: fn elided(n: i32) -> i32 {
// CHECK-NEXT:     n * 3i32
// CHECK-NEXT: }
// CHECK-NEXT: fn c_main() -> i32 {
// CHECK-NOT:  allow(unused_assignments)
