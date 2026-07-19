// REQUIRES: cargo
// C99-29: differential end-to-end test for float literal spellings —
// hexadecimal float constants (C99 6.4.4.2), leading/trailing-dot and
// exponent decimal forms, and the f/F suffixes — printed with %f (six
// decimals, the C99-47 subset). Every value below is an exact binary
// value (a multiple of 2^-6), so C and Rust print identical digits and
// diff covers the observable behavior byte for byte.
// --release is load-bearing: debug Rust panics on integer overflow where
// C wraps.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/float_literals > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

double g_hex = 0x1.4p2; /* 5.0 through the file-scope fold */

double combine(double a, double b) {
  return a * 0x1p-2 + b * 0.5;
}

int main(void) {
  /* Hexadecimal doubles: 12.0, 0.25, 10.5, 0.015625. */
  printf("a=%f\n", 0x1.8p3);
  printf("b=%f\n", 0x1p-2);
  printf("c=%f\n", 0xA.8p0);
  printf("d=%f\n", 0x1p-6);
  /* Hexadecimal floats (f/F suffix, capital X/P): 3.0f, 16.0f. */
  float hf = 0x1.8p1f;
  printf("e=%f\n", hf);
  printf("g=%f\n", 0X1P4F);
  /* Decimal forms: leading dot, trailing dot, exponents, F suffix. */
  printf("h=%f\n", .5);
  printf("i=%f\n", 5.);
  printf("j=%f\n", 1e3);
  printf("k=%f\n", 1.5e2);
  float df = 0.25F;
  printf("l=%f\n", df);
  /* Mixed arithmetic over the spellings, plus the folded global. */
  printf("m=%f\n", combine(0x1.8p3, .5));
  printf("n=%f\n", g_hex);
  return 0;
}
