// REQUIRES: cargo
// C99-48: differential end-to-end test for the hosted sin mapping: C's
// libm sin against Rust's f64::sin (both resolve to the platform libm on
// this target, and printf's six %f decimals absorb any sub-ulp
// difference; a mismatch here is a real finding, not noise to fudge).
// Values cover zero, positive and negative arguments, an int argument
// going through the usual conversion to double, a near-pi argument whose
// sine rounds to 0.000000, and a swept range accumulating through a
// variable.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native -lm
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/math_sin > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <math.h>

int printf(const char *, ...);

int main(void) {
  printf("%f\n", sin(0.0));
  printf("%f\n", sin(1.0));
  printf("%f\n", sin(-1.5));
  printf("%f\n", sin(2));
  printf("%f\n", sin(3.141592653589793));
  printf("%f\n", sin(100.0));

  double x = -2.0;
  int i = 0;
  while (i < 9) {
    printf("sin(%f)=%f\n", x, sin(x));
    x += 0.5;
    i += 1;
  }

  double nested = sin(sin(1.0)) + sin(0.5) * sin(0.25);
  printf("%f\n", nested);
  return 0;
}
