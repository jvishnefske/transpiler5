// REQUIRES: cargo
// FR-142 (defect): a DEFERRED binding (`let off: i64;`, initializer dropped)
// used as the INDEX of a MUTABLE slice borrow -- `&mut s[off as usize..]`, the
// ubiquitous "advance a cursor into the caller's buffer" shape -- was emitted
// `let mut` and never reassigned. Emitted crates deny `unused_mut`, so the
// crate did not compile at all:
//   error: variable does not need to be mutable  --> let mut v7: i64;
// exit 0 from the transpiler, an unbuildable crate, and no diagnostic.
// `lift_shape` below is the FR entry's reproducer; before the fix the
// `--build` line alone fails this whole file.
//
// The cause: the post-init-mutation scan asked `sliceOf.getIsMut()` without
// asking WHICH of that op's two operands (`$base`, `$index`) the binding is, so
// a binding serving as the pure-READ start index was scored as a mutable borrow
// OF ITSELF.
//
// A `mut` decision cannot by itself change behaviour, so `--build` is only the
// buildability oracle -- though an exact one in BOTH directions (a missing
// `mut` is rustc E0384, a stale one is the `unused_mut` deny). The CORRECTNESS
// oracle is the stdout diff, and it is required here because clearing the flag
// makes FR-61b's if-expression lift newly eligible: `let mut off: i64; if c {
// off = a } else { off = b }` collapses to `let off: i64 = if c { a } else
// { b };`. That moves initializers across a control-flow join, and a dropped,
// duplicated or reordered one compiles perfectly and prints the wrong number.
// Every value below -- the buffer bytes, the branch conditions and the
// resulting offsets -- derives from `argc`, so no constant folding can
// precompute a result or let a lost store hide behind a folded constant, and
// the three runs take BOTH sides of every `avail < len` branch.
//
// The last two functions are the OVER-RELAXATION guards, and they matter as
// much as the reproducer: `loop_index`'s cursor is genuinely carried across
// loop iterations, and `base_shape`'s deferred binding is the BASE of a mutable
// borrow taken after its single initializing write. Both must keep their `mut`
// -- E0384 and E0596 respectively if the narrowed test reaches too far.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/deferred_mut_slice_index > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/deferred_mut_slice_index a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b c d e > %t.n5.out && %t.crate/target/release/deferred_mut_slice_index a b c d e > %t.r5.out
// RUN: diff %t.n5.out %t.r5.out
//
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s

#include <stdio.h>

struct Buf {
  char data[16];
};

static long grow(unsigned long n) { return (long)(n % 5) + 1; }

static void sink(char *p) { printf("[%d]\n", (int)p[0]); }

static void bump(char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    p[i] = (char)(p[i] + 1);
}

// THE REPRODUCER. The binding is written once per arm and then merely READ as
// the slice start index, so it needs no `mut` -- and with the `mut` gone, the
// if-expression lift applies.
// CHECK-LABEL: fn tu0_lift_shape
// CHECK-NOT:     let mut
// CHECK:         let [[OFF:v[0-9]+]]: i64 = if avail < len {
// CHECK:         &mut (*s)[
// CHECK-SAME:    [[OFF]] as usize..];
static long lift_shape(char *s, unsigned long avail, unsigned long len) {
  long off;
  if (avail < len) {
    off = grow(len - avail);
  } else {
    off = 0;
  }
  sink(s + off);
  return off;
}

// The same defect WITHOUT the second-order lift: the arms are a `switch`, and
// the if-expression lift only ever fires on an `if`. The two-program-point
// rendering therefore survives, which makes this arm the isolated pin on the
// `mut` decision itself -- the reproducer above cannot distinguish "the `mut`
// went away" from "the whole binding was rewritten into an if-expression".
// CHECK-LABEL: fn tu0_switch_index
// CHECK:         let [[SOFF:v[0-9]+]]: i64;
// CHECK-NEXT:    match
static long switch_index(char *s, unsigned long avail, unsigned long len) {
  long off;
  printf("[gap %d]\n", (int)len);
  switch (len % 3) {
  case 0:
    off = grow(len - avail);
    break;
  case 1:
    off = 2;
    break;
  default:
    off = 0;
    break;
  }
  sink(s + off);
  return off;
}

// OVER-RELAXATION GUARD 1: a slice index that is genuinely carried across loop
// iterations. The loop carry -- not the borrow -- is what makes it `mut`, and
// the narrowed borrow test must leave that alone. Emitting this without `mut`
// is rustc E0384, so `--build` is the pin; the walking cursor also makes the
// stdout diff sensitive to the offset actually used at each iteration.
//
// FR-61f-c slice 1 moved this function's RENDERING forward and the pin moves
// with it, unweakened. The body reads the pointer parameter `s`, which used to
// force the whole loop down the cf `while` lowering; `s` is body-invariant, so
// the loop now lifts to `emitrust.for` and the counter is the range head
// instead of a loop-carried value -- `loop { ... }` with a carried `v17`
// became `for _i_1 in 0u64..len`. The cursor is therefore no longer an
// SSA-carried temporary but the named place `off`, which is exactly why the
// binding name and the initializer form below had to change. What the guard
// asserts has not: the binding that indexes the mutable borrow still carries
// `mut`, and it is still the SAME binding at both program points.
// CHECK-LABEL: fn tu0_loop_index
// CHECK:         let mut [[CUR:[A-Za-z_0-9]+]]: i64;
// CHECK:         for {{.*}} in 0u64..len {
// CHECK:         &mut (*s)[
// CHECK-SAME:    [[CUR]] as usize..];
static long loop_index(char *s, unsigned long avail, unsigned long len) {
  long off;
  unsigned long i;
  if (avail < len) {
    off = grow(len - avail);
  } else {
    off = 0;
  }
  for (i = 0; i < len; i++) {
    sink(s + off);
    off = (off + 1) % 4;
  }
  return off;
}

// OVER-RELAXATION GUARD 2: the deferred binding is the BASE of the mutable
// borrow, not its index -- `&mut tmp.data[..]` really does mutate `tmp` after
// its single initializing write. Dropping this `mut` is rustc E0596, and the
// mutation is observable in stdout because the bumped bytes are printed.
// CHECK-LABEL: fn tu0_base_shape
// CHECK:         let mut tmp: Buf;
// CHECK:         &mut tmp.data[
static long base_shape(int flag, struct Buf *src) {
  struct Buf tmp;
  if (flag) {
    tmp = *src;
  } else {
    tmp = *src;
  }
  bump(tmp.data, 4);
  return (long)tmp.data[0] + (long)tmp.data[3];
}

static void run(struct Buf *b, int seed) {
  printf("%ld %ld %ld\n", lift_shape(b->data, (unsigned long)seed, 4UL),
         switch_index(b->data, (unsigned long)seed, 6UL),
         loop_index(b->data, (unsigned long)seed, 9UL));
}

int main(int argc, char **argv) {
  struct Buf b;
  struct Buf c;
  int i;
  for (i = 0; i < 16; i++)
    b.data[i] = (char)(i * 3 + argc);
  for (i = 0; i < 16; i++)
    c.data[i] = (char)(i * 5 + argc * 2);
  run(&b, argc);
  printf("%ld\n", base_shape(argc & 1, &c));
  return 0;
}
