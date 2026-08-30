// REQUIRES: cargo
// FR-155 (silent miscompile): adding a DEAD `goto`/label to a function
// changed its answer. The invariant this file pins is:
//
//   a place-backed scalar declared inside a loop body must be
//   re-initialized on every iteration, and the presence of a label
//   anywhere in the function must not change that.
//
// Mechanism: `createVariablePlace` hoists the `emitrust.variable` op to the
// function entry block whenever the function has a label (so a goto over a
// declaration cannot leave a later use undominated), while FR-61f attaches
// a compile-time-constant initializer to the variable op ITSELF to avoid
// clippy::needless_late_init. Both firing at once carried the initializer
// out of the loop with the declaration and dropped the per-iteration reset:
// `let mut s: i32 = 0;` moved to function entry, so `s` accumulated across
// iterations instead of resetting. `cargo build` was perfectly happy --
// only the stdout diff against the clang-built native can see this class of
// defect, which is why this test is the oracle and the FileCheck pin in
// test/Import/C/label-hoist-reinit.c is only the tripwire.
//
// Every labelled function is paired with a byte-identical label-free
// control that must keep printing the same number: a label is not allowed
// to be observable. Every bound and value derives from `argc` so no
// constant folding can precompute an answer and hide a dropped store, and
// the arithmetic is chosen so a lost reset changes the PRINTED value --
// measured, not inferred. At argc==1 clang prints
//   reset=3  guarded=24  nested=27  shadow=18
// and the unfixed transpiler printed
//   reset=4  guarded=58  nested=75  shadow=19
// while every `nolabel=` control stayed correct on both sides.
// Deterministic, no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/label_hoist_reinit > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a > %t.native2.out
// RUN: %t.crate/target/release/label_hoist_reinit a > %t.rust2.out
// RUN: diff %t.native2.out %t.rust2.out
// RUN: %t.native a b c > %t.native4.out
// RUN: %t.crate/target/release/label_hoist_reinit a b c > %t.rust4.out
// RUN: diff %t.native4.out %t.rust4.out
// RUN: %t.native a b c d e f g > %t.native8.out
// RUN: %t.crate/target/release/label_hoist_reinit a b c d e f g > %t.rust8.out
// RUN: diff %t.native8.out %t.rust8.out

#include <stdio.h>

/* 1. THE REPRODUCER. `s` is declared in the loop body and reset to 0 each
   iteration; the `goto` is never taken and exists only to put a label in
   the function. n=3 gives 0+1+2=3 when the reset happens and 0+1+3=4 when
   the initializer is hoisted out with the declaration. */
static int sum_reset_labelled(int n) {
  int total = 0;
  for (int i = 0; i < n; i++) {
    int s = 0;
    s += i;
    total += s;
  }
  if (total < 0)
    goto done;
done:
  return total;
}

/* 2. The control: byte-identical body with no label at all. */
static int sum_reset_nolabel(int n) {
  int total = 0;
  for (int i = 0; i < n; i++) {
    int s = 0;
    s += i;
    total += s;
  }
  return total;
}

/* 3. The accumulate is SKIPPED on some iterations, so a lost reset leaks
   into later iterations instead of coincidentally cancelling out: with the
   reset, n=6 accumulates 2+4+8+10 = 24; without it, 2+6+20+30 = 58. */
static int sum_guarded_labelled(int n) {
  int total = 0;
  for (int i = 0; i < n; i++) {
    int s = 0;
    s += i * 2;
    if (i % 3 != 0)
      total += s;
  }
  if (total < 0)
    goto done;
done:
  return total;
}

static int sum_guarded_nolabel(int n) {
  int total = 0;
  for (int i = 0; i < n; i++) {
    int s = 0;
    s += i * 2;
    if (i % 3 != 0)
      total += s;
  }
  return total;
}

/* 4. The same skip written as a `continue`. Measured: a `continue`
   anywhere in the nest disqualifies the range-`for` lift, so this shape is
   promoted to SSA by mem2reg and never reaches the place path at all --
   it matched the native both before and after the fix. It is kept as a
   non-regression companion and as a tripwire: if range-`for` eligibility is
   ever widened to admit `continue`, this case starts exercising the fence
   and the diff below is already watching it. */
static int sum_continue_labelled(int n) {
  int total = 0;
  for (int i = 0; i < n; i++) {
    int s = 0;
    s += i * 2;
    if (i % 3 == 0)
      continue;
    total += s;
  }
  if (total < 0)
    goto done;
done:
  return total;
}

static int sum_continue_nolabel(int n) {
  int total = 0;
  for (int i = 0; i < n; i++) {
    int s = 0;
    s += i * 2;
    if (i % 3 == 0)
      continue;
    total += s;
  }
  return total;
}

/* 5. The place is declared in the INNER loop body, so a lost reset
   compounds across both loops rather than merely across one: n=3 gives 27
   with the reset and 75 without. */
static int sum_nested_labelled(int n) {
  int total = 0;
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < 3; j++) {
      int s = 1;
      s += i + j;
      total += s;
    }
  }
  if (total < 0)
    goto done;
done:
  return total;
}

static int sum_nested_nolabel(int n) {
  int total = 0;
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < 3; j++) {
      int s = 1;
      s += i + j;
      total += s;
    }
  }
  return total;
}

/* 6. TWO places named `s`: the outer one is read by the second range-`for`
   body so it is place-backed too, and hoisting puts both in the entry
   block, where the emitter renames one (`s` / `s_1`). The inner reset must
   land inside the first loop and must land on the INNER place -- putting it
   on the outer one, or dropping it, both change the printed number. */
static int sum_shadow_labelled(int n) {
  int s = 100;
  int total = 0;
  for (int i = 0; i < n; i++) {
    int s = 2;
    s += i;
    total += s;
  }
  for (int k = 0; k < n; k++)
    total += s / 50 + k;
  if (total < 0)
    goto done;
done:
  return total;
}

static int sum_shadow_nolabel(int n) {
  int s = 100;
  int total = 0;
  for (int i = 0; i < n; i++) {
    int s = 2;
    s += i;
    total += s;
  }
  for (int k = 0; k < n; k++)
    total += s / 50 + k;
  return total;
}

/* 7. THE HOIST IS STILL REQUIRED. `u` is read by a range-`for` body (so it
   is place-backed) and its declaration is jumped OVER, which is exactly the
   shape the entry-block hoist exists for: without the hoist the
   post-label assignment would not dominate the use and the emitted crate
   would not compile. With the fix the initializer `u = 9` stays at the
   (skipped) declaration point, matching C, and `u = 3` is what is read. */
static int skip_place_decl(int n) {
  int acc = 0;
  if (n >= 0)
    goto skip;
  int u = 9;
skip:
  u = 3;
  for (int i = 0; i < n; i++)
    acc += u + i;
  return acc;
}

int main(int argc, char **argv) {
  int n = argc + 2; /* every bound and value derives from argc */
  printf("reset=%d nolabel=%d\n", sum_reset_labelled(n), sum_reset_nolabel(n));
  printf("guarded=%d nolabel=%d\n", sum_guarded_labelled(n + 3),
         sum_guarded_nolabel(n + 3));
  printf("continue=%d nolabel=%d\n", sum_continue_labelled(n + 3),
         sum_continue_nolabel(n + 3));
  printf("nested=%d nolabel=%d\n", sum_nested_labelled(n),
         sum_nested_nolabel(n));
  printf("shadow=%d nolabel=%d\n", sum_shadow_labelled(n),
         sum_shadow_nolabel(n));
  printf("skip=%d\n", skip_place_decl(n));
  return 0;
}
