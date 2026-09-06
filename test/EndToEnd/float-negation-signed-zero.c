// REQUIRES: cargo
// Differential end-to-end test pinning that C's unary minus on a FLOATING
// operand is IEEE-754 negation (a sign-bit flip), not a subtraction from
// zero. The two disagree on exactly one input class, and it is invisible
// to any ASCII eyeball that only checks the magnitude: `0.0 - 0.0` is
// +0.0 while `-(0.0)` is -0.0, so printf renders "0.000000" where C
// renders "-0.000000". A NaN operand diverges the other way: `0.0 - x`
// PROPAGATES the operand's NaN bit pattern (sign included), while `-x`
// flips its sign bit, so the x86 default negative QNaN produced by
// `inf - inf` prints "nan" in C and "-nan" under the subtraction
// spelling. Infinities were MEASURED not to diverge (`0.0 - inf` is
// -inf, same as `-inf`) but are pinned here anyway so a future
// re-spelling of the lowering cannot break them silently.
//
// Every operand derives from argc, so no constant fold can pre-compute a
// sign and hide a miscompile; the second RUN pair re-seeds through extra
// argv words to move argc off 1. The literal `-0.0` line pins the
// separate CONSTANT path (the importer folds it at import and it was
// already correct) so the fix cannot regress it, and the integer line
// pins that unary minus on an int keeps its `0 - x` lowering — that
// spelling is correct on integers, where there is no signed zero.
//
// `cargo build` success proves nothing here: both spellings compile.
// The stdout diff against the clang-built native is the oracle.
// --release is load-bearing: debug Rust panics on integer overflow
// where C wraps.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/float_negation_signed_zero > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b c > %t.native4.out
// RUN: %t.crate/target/release/float_negation_signed_zero a b c > %t.rust4.out
// RUN: diff %t.native4.out %t.rust4.out

int printf(const char *, ...);

struct Pair {
  double d;
  float f;
};

static double negd(double x) { return -x; }
static float negf(float x) { return -x; }

/* argc == 1 makes this exactly +0.0; argc == 4 makes it 3.0. */
static double seed(int n) { return (double)(n - 1); }

int main(int argc, char **argv) {
  double z = (double)(argc - 1);
  float zf = (float)(argc - 1);

  struct Pair p;
  p.d = z;
  p.f = zf;

  /* Runtime overflow, so the infinities are not constant-folded. The
     second pair exists only so the NaN below is `a - b` with distinct
     operands: `inf - inf` is the same expression twice and clippy's
     deny-by-default eq_op refuses to lint the emitted crate. */
  double big = 1e308 * (double)argc;
  double inf = big * 10.0;
  double inf2 = big * 100.0;
  float bigf = 1e38f * (float)argc;
  float inff = bigf * 10.0f;
  float inff2 = bigf * 100.0f;

  int i = argc - 1;

  /* A plain variable, at both widths, through all three conversions. */
  printf("var   %f %g %e\n", -z, -z, -z);
  printf("varf  %f %g %e\n", (double)-zf, (double)-zf, (double)-zf);

  /* A call result, at both widths. */
  printf("call  %f %f\n", -seed(argc), (double)-negf(zf));

  /* Double negation must cancel back to +0.0. */
  printf("twice %f %f\n", -negd(z), (double)-negf(-zf));

  /* A struct member, at both widths. */
  printf("memb  %f %f\n", -p.d, (double)-p.f);

  /* A parenthesised sub-expression (the operand needs its own parens). */
  printf("expr  %f %f\n", -(z + 0.0), -(z * 2.0));

  /* The constant path: already correct, pinned so it stays that way. */
  printf("lit   %f %g %e\n", -0.0, -0.0, -0.0);

  /* Infinities, at both widths, through all three conversions. */
  printf("inf   %f %g %e\n", -inf, -inf, -inf);
  printf("inff  %f %g %e\n", (double)-inff, (double)-inff, (double)-inff);

  /* NaN sign: inf minus inf is the negative default QNaN on x86, so the
     negation must print "nan" (positive), not "-nan". This is the OTHER
     divergence of the `0.0 - x` spelling: subtraction PROPAGATES the
     operand's NaN bit pattern, sign included, where `-x` flips it. */
  printf("nan   %f %f\n", -(inf - inf2), (double)-(inff - inff2));

  /* Integer unary minus is UNCHANGED by the float fix. */
  printf("int   %d %d\n", -i, -argc);

  return 0;
}
