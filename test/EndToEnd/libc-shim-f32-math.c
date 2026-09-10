// REQUIRES: cargo
// FR-224: differential regression for the BINARY32 half of the hosted
// <math.h> subset. The invariant this pins is that `sqrtf`/`fabsf`/
// `floorf`/`ceilf` are as bit-exact as the f64 forms the table already
// admitted -- IEEE-754 mandates fabs/floor/ceil exactly and sqrt
// correctly rounded in binary32 by the same clauses that mandate them in
// binary64 -- so byte-identical stdout against the clang-built native is
// the whole claim, and `%.9g` is used because it round-trips every f32
// and therefore cannot hide a last-bit disagreement the way `%f` would.
//
// WHY THE INPUTS ARE WHAT THEY ARE. The interesting f32 sqrt inputs are
// the ones where the correctly-rounded binary32 result is NOT the
// binary64 result rounded down -- i.e. where computing in double and
// narrowing would give a different answer (a "double rounding" miss).
// 2.0f is the classic; the others cover exact squares (no rounding at
// all), subnormal-adjacent smalls, and a large value. floorf/ceilf run
// on both signs and on exact integers, where they must return the input
// including its SIGN OF ZERO -- `floorf(-0.0f)` is -0.0, and a shim that
// went through an integer would lose that (FR-200 measured exactly this
// class of sign-of-zero loss for negation). fabsf is checked on -0.0f
// for the same reason.
//
// `expf` is deliberately ABSENT: it stays a located rejection with
// `exp`/`log`/`pow` (test/Import/C/libc-shim-invalid.c pins the wording).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name libc_shim_f32_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native -lm
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/libc_shim_f32_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <math.h>
#include <stdio.h>

static void show(const char *label, float value) {
  printf("%s %.9g\n", label, (double)value);
}

int main(void) {
  float roots[6] = {2.0f, 4.0f, 0.5f, 1e-20f, 1e20f, 0.0f};
  int i;
  for (i = 0; i < 6; i++)
    show("sqrtf", sqrtf(roots[i]));

  float mags[5] = {-2.5f, 2.5f, -0.0f, 0.0f, -1e-30f};
  for (i = 0; i < 5; i++)
    show("fabsf", fabsf(mags[i]));

  float rounds[8] = {1.7f, -1.7f, 2.0f, -2.0f, 0.0f, -0.0f, 0.5f, -0.5f};
  for (i = 0; i < 8; i++) {
    show("floorf", floorf(rounds[i]));
    show("ceilf", ceilf(rounds[i]));
  }

  /* The f64 forms still work: the widening must not have displaced them. */
  printf("f64 %.17g %.17g %.17g\n", sqrt(2.0), floor(-1.5), fabs(-0.0));
  return 0;
}
