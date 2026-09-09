// FR-215 (clippy::needless_late_init): the COND-EXPRESSION BINDING generalised
// from `emitrust.if` to `emitrust.switch`, and from adjacency to FR-132's scope
// rule -- BYTE-DIFFED against the clang-built native.
//
// The fold rewrites control flow into an expression: a `switch` statement whose
// every arm assigns one binding becomes `let x: T = match d { .. };`, and the
// declaration sinks from its own program point down to the `match`. `cargo
// build` cannot see any of the ways that goes wrong -- a dropped arm, a wrong
// arm value, a side effect that moved across the binding, a `break` arm folded
// into a value it never had -- because every one of them compiles perfectly and
// prints the wrong number. So every shape below is byte-diffed against the
// clang native at several argument counts. The seeds come from `argc` so
// nothing constant-folds, and each case is reached at some seed.
//
// The gaps and the arms deliberately carry PRINTING side effects: the
// declaration moves, the statements must not, and the interleaved stdout is the
// pin for that. A `break` arm and an arm with no assignment at all are here so
// the REFUSALS are exercised at runtime too, not only in the emitted text --
// folding either would have to invent a value.
//
// The `--build` line is a second, weaker check: emitted crates deny(unused_mut)
// and rustc's own E0381/E0384 are the safe failure direction, so a `mut` or a
// definite-assignment fact the fold got wrong fails the build outright.
//
// REQUIRES: cargo
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/cond_expr_binding > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/cond_expr_binding a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b > %t.n2.out && %t.crate/target/release/cond_expr_binding a b > %t.r2.out
// RUN: diff %t.n2.out %t.r2.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/cond_expr_binding a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out
// RUN: %t.native a b c d > %t.n4.out && %t.crate/target/release/cond_expr_binding a b c d > %t.r4.out
// RUN: diff %t.n4.out %t.r4.out
//
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s

int printf(const char *, ...);

int side(int v) {
  printf("side(%d)\n", v);
  return v * 7 + 1;
}

// THE TAIL FOLD, and the reason it is not optional: without it this function
// renders `let r: i32 = match .. {}; r` and merely TRADES `needless_late_init`
// for `clippy::let_and_return`. The whole body must be the bare `match`.
// CHECK-LABEL: fn classify
// CHECK-NEXT:    match
// CHECK-NOT:     let
int classify(int n) {
  int r;
  switch (n % 4) {
  case 0:
    r = 11;
    break;
  case 1:
    r = 22;
    break;
  case 2:
    r = 33;
    break;
  default:
    r = 44;
    break;
  }
  return r;
}

// The binding is READ after the match, so the `let` survives -- bound at the
// `match`, in the OUTER block, still in scope for both reads below it.
// CHECK-LABEL: fn classify_then_use
// CHECK-NEXT:    let [[R:v[0-9]+]]: i32 = match
// CHECK:         };
// CHECK-NEXT:    [[R]] * 2i32 + [[R]]
int classify_then_use(int n) {
  int r;
  switch (n % 3) {
  case 0:
    r = 5;
    break;
  case 1:
    r = 6;
    break;
  default:
    r = 7;
    break;
  }
  return r * 2 + r;
}

// SIDE EFFECTS INSIDE THE ARMS keep their position and their order: the
// `printf` must run BEFORE the value it precedes is produced, on exactly the
// arm that is taken. The interleaved stdout is the pin.
// CHECK-LABEL: fn arm_effects
// CHECK:         let {{v[0-9]+}}: i32 = match
int arm_effects(int n) {
  int r;
  switch (n % 3) {
  case 0:
    printf("arm0\n");
    r = side(n);
    break;
  case 1:
    printf("arm1\n");
    r = side(n + 10);
    break;
  default:
    printf("arm-default\n");
    r = side(n + 100);
    break;
  }
  printf("after switch\n");
  return r + 1;
}

// A FALL-THROUGH ARM THAT ASSIGNS NOTHING IN C STILL FOLDS, and finding out
// why is the reason this leg is here. The importer's SCF destruction
// materialises the incoming value on that path -- the emitted IR really does
// end case 0 with `assign r = 0` -- so every arm tail-assigns and the match is
// a legitimate expression. Measured on the IR, not assumed: the consequence of
// getting it wrong is `r` silently taking arm 1's value at seed 3, which only
// the byte-diff below can see.
// CHECK-LABEL: fn silent_arm
// CHECK-NEXT:    match
// CHECK:         println!("silent");
// CHECK-NEXT:    0i32
int silent_arm(int n) {
  int r = 0;
  switch (n % 3) {
  case 0:
    printf("silent\n");
    break;
  case 1:
    r = 61;
    break;
  default:
    r = 62;
    break;
  }
  return r;
}

// REFUSES, exercised at runtime. Finding a C source that the SWITCH fold
// refuses takes some doing, and that fact is itself worth recording: the
// importer's mem2reg gives each binding a single write per region, so the
// obvious "assign again after the switch" reads back as a fresh SSA value and
// folds anyway. What does refuse is a binding that must stay a real memory
// place -- here its address is taken -- which `computeDeferredInits` marks as
// needing `mut`, and a `mut` binding is not the arms-assign-once shape. The
// two-program-point form survives and the `*p += 100` must still be observed.
// (The fold's own arm-shape refusals -- a diverging arm, an arm that yields
// nothing -- are NOT reachable from C at all, because SCF destruction
// materialises a value on every path; they are pinned on hand-written IR in
// test/Target/Rust/cond-expr-binding.mlir.)
// CHECK-LABEL: fn addressed
// CHECK-NEXT:    let mut [[R:[a-z_0-9]+]]: i32;
// CHECK-NEXT:    match
// CHECK:         [[R]] += 100i32;
int addressed(int n) {
  int r = 0;
  int *p = &r;
  switch (n % 3) {
  case 0:
    r = 1;
    break;
  case 1:
    r = 2;
    break;
  default:
    r = 3;
    break;
  }
  *p += 100;
  return r;
}

// SUNK: the `if`'s condition is an inlined comparison, so in the IR the
// `emitrust.cmp` sits BETWEEN the declaration and the `emitrust.if` and the old
// `getNextNode()` rule never saw the `if` at all. The arms CALL rather than
// yield constants on purpose -- two constant arms fold to an `emitrust.select`
// upstream and never reach this fold, so a constant version of this leg would
// silently test nothing.
// CHECK-LABEL: fn sunk_cmp
// CHECK-NEXT:    let {{v[0-9]+}}: i32 = if n > 2i32 {
int sunk_cmp(int n) {
  int r;
  if (n > 2)
    r = side(n);
  else
    r = side(n + 5);
  return r + n;
}

// SUNK past a REAL statement, which is the harder half of the same rule: the
// gap here renders a `let` of its own AND a printing side effect. The
// declaration sinks past it; the statement must not move, and the interleaved
// stdout is what proves it did not.
// CHECK-LABEL: fn sunk_statement
// CHECK:         let [[G:[a-z_0-9]+]]: i32 = side(
// CHECK-NEXT:    let {{v[0-9]+}}: i32 = if [[G]] > 20i32 {
int sunk_statement(int n) {
  int r;
  int g = side(n + 1);
  if (g > 20)
    r = side(g);
  else
    r = side(g + 3);
  printf("g=%d\n", g);
  return r + g;
}

// A NESTED switch inside a switch arm: the inner fold happens inside the outer
// arm's body, and the outer arm's tail is still its own assignment.
// CHECK-LABEL: fn nested
int nested(int n) {
  int outer;
  switch (n % 2) {
  case 0: {
    int inner;
    switch (n % 3) {
    case 0:
      inner = 1;
      break;
    case 1:
      inner = 2;
      break;
    default:
      inner = 3;
      break;
    }
    printf("inner=%d\n", inner);
    outer = inner * 100;
    break;
  }
  default:
    outer = -1;
    break;
  }
  return outer;
}

// An enum-typed binding: the fold must carry the binding's TYPE, not the
// discriminator's, and the arm literals stay in the discriminator's type.
enum Kind { KA = 0, KB = 1, KC = 2 };

// CHECK-LABEL: fn pick
int pick(int n) {
  enum Kind k;
  switch (n % 3) {
  case 0:
    k = KA;
    break;
  case 1:
    k = KB;
    break;
  default:
    k = KC;
    break;
  }
  return (int)k * 1000;
}

int main(int argc, char **argv) {

  int seed = argc;
  printf("classify %d\n", classify(seed));
  printf("classify_then_use %d\n", classify_then_use(seed));
  printf("arm_effects %d\n", arm_effects(seed));
  printf("silent_arm %d\n", silent_arm(seed));
  printf("addressed %d\n", addressed(seed));
  printf("sunk_cmp %d\n", sunk_cmp(seed));
  printf("sunk_statement %d\n", sunk_statement(seed));
  printf("nested %d\n", nested(seed));
  printf("pick %d\n", pick(seed));
  return 0;
}
