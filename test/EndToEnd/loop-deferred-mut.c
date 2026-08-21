// REQUIRES: cargo
// FR-105 (defect): a binding whose initializer is DEFERRED (`let j: u32;`) and
// which is then assigned inside a loop was emitted WITHOUT `mut`, so the
// emitted crate did not compile at all: `error[E0384]: cannot assign twice to
// immutable variable`. `f_repro` below is the eight-line reproducer from the
// FR entry verbatim; before the fix this whole file cannot be built, which is
// why the defect is worth an EndToEnd test rather than a golden alone.
//
// The `mut` predicate is exact in BOTH directions, and this file gates both.
// Emitted crates deny `unused_mut`, so a binding that is marked mutable
// without needing it fails the build just as loudly as one that needs it and
// is not: `f_break_only`, `f_before_loops` and `f_early_return` write their
// deferred binding on a path that never reaches a back edge and must keep the
// bare `let`. A successful `--build` is therefore simultaneously the
// under-marking oracle (E0384) and the over-marking oracle (unused_mut).
//
// Neither of those is a CORRECTNESS oracle, though: a `mut` decision cannot
// change behaviour, but the analysis that makes it shares `analyzeSeq` with
// deferred-init and dead-store elision, and a mistake THERE drops a store
// silently. Only the stdout diff against the clang-built native can see that.
// Every value below derives from `argc` -- the loop bounds, the stored bytes,
// and the branch conditions -- so no constant folding can precompute a result
// and no dropped store can hide behind a folded constant. Each shape is run at
// several argc values so both sides of every in-loop branch are taken.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/loop_deferred_mut > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a > %t.native2.out
// RUN: %t.crate/target/release/loop_deferred_mut a > %t.rust2.out
// RUN: diff %t.native2.out %t.rust2.out
// RUN: %t.native a b c > %t.native4.out
// RUN: %t.crate/target/release/loop_deferred_mut a b c > %t.rust4.out
// RUN: diff %t.native4.out %t.rust4.out
// RUN: %t.native a b c d e f g h i > %t.native10.out
// RUN: %t.crate/target/release/loop_deferred_mut a b c d e f g h i > %t.rust10.out
// RUN: diff %t.native10.out %t.rust10.out

#include <stdio.h>

/* 1. The FR-105 reproducer, verbatim: `j` is deferred, written under the
   lifted loop's condition, and its write recurs. Needs `mut`. */
static void f_repro(unsigned n, unsigned char *out) {
  unsigned i, j;
  for (i = 0; i < n; ++i) { j = i * 4; out[j] = (unsigned char)i; }
}

/* 2. Two bindings written by ONE C statement in one loop -- tiny-AES-c's
   `j = i * 4; k = (i - Nk) * 4;` (aes.c:211), which emitted `let j: u32;`
   on one line and `let mut k: u32;` on the next. */
static unsigned f_two_bindings(unsigned n) {
  unsigned i, j, k, s = 0;
  for (i = 4; i < n; ++i) { j = i * 4; k = (i - 4) * 4; s += j + k; }
  return s;
}

/* 3. Write then `break`: the back edge is unreachable from the write, so the
   assignment happens at most once and the bare `let` is the only form rustc
   accepts. */
static unsigned f_break_only(unsigned n) {
  unsigned i, j = 0;
  unsigned found;
  for (i = 0; i < n; ++i) { if (i == 3) { found = i * 7; j = found; break; } }
  return j;
}

/* 4. Write then `return`: divergence, not a back edge -- bare `let`. */
static unsigned f_early_return(unsigned n) {
  unsigned i, r;
  for (i = 0; i < n; ++i) { if (i == 2) { r = i * 11; return r; } }
  return 0;
}

/* 5. Write then `continue`: the write never falls through the body, yet it
   DOES reach the back edge, so this one needs `mut`. */
static unsigned f_continue(unsigned n) {
  unsigned i, t, s = 0;
  for (i = 0; i < n; ++i) {
    if (i & 1) { t = i * 3; s += t; continue; }
    s += 1;
  }
  return s;
}

/* 6. A `switch` arm that writes and leaves the match by its own `break`
   still reaches the loop's back edge. */
static unsigned f_switch(unsigned n) {
  unsigned i, v, s = 0;
  for (i = 0; i < n; ++i) {
    switch (i % 3) {
      case 0: v = i * 13; s += v; break;
      case 1: s += 100; break;
      default: s += 2; break;
    }
  }
  return s;
}

/* 7. do/while: unconditional write, tail-conditional back edge. */
static unsigned f_do_while(unsigned n) {
  unsigned i = 0, q, s = 0;
  do { q = i * 5; s += q; ++i; } while (i < n);
  return s;
}

/* 8. Nested loops: the inner write reaches the INNER back edge. */
static unsigned f_nested(unsigned n) {
  unsigned i, a, b, s = 0;
  for (i = 0; i < n; ++i) {
    for (a = 0; a < 3; ++a) { b = a * i + 1; s += b; }
  }
  return s;
}

/* 9. THE OVER-MARKING GUARD: `base`'s only write is BEFORE the loops, which
   merely read it. An ANY-write flag that is not reset at each loop-body entry
   leaks that write onto the inner loop's exit and marks `base` mutable --
   `deny(unused_mut)` then fails this build. */
static unsigned f_before_loops(unsigned n) {
  unsigned i, base, s = 0;
  base = n * 2;
  for (i = 0; i < n; ++i) {
    unsigned a;
    for (a = 0; a < 2; ++a) { if (a) break; }
    s += base;
  }
  return s;
}

/* 10. `while` head instead of a counting `for`. */
static unsigned f_while(unsigned n) {
  unsigned i = 0, w, s = 0;
  while (i < n) { if (i % 2 == 0) { w = i * 9; s += w; } ++i; }
  return s;
}

/* 11. A `goto` to the loop tail -- the same back edge by another spelling. */
static unsigned f_goto_tail(unsigned n) {
  unsigned i, g, s = 0;
  for (i = 0; i < n; ++i) {
    if (i > 5) { g = i * 17; s += g; goto next; }
    s += 3;
  next:;
  }
  return s;
}

int main(int argc, char **argv) {
  unsigned char buf[64];
  unsigned n = (unsigned)argc;   /* every bound and value derives from argc */
  unsigned k;
  for (k = 0; k < 64; ++k) buf[k] = (unsigned char)(k + n);
  f_repro(n + 3u, buf);
  for (k = 0; k < 64; ++k) printf("%u ", (unsigned)buf[k]);
  printf("\n");
  printf("%u %u %u %u %u %u %u %u %u %u\n", f_two_bindings(n + 6u),
         f_break_only(n + 6u), f_early_return(n + 6u), f_continue(n + 6u),
         f_switch(n + 6u), f_do_while(n + 6u), f_nested(n + 6u),
         f_before_loops(n + 6u), f_while(n + 6u), f_goto_tail(n + 6u));
  return 0;
}
