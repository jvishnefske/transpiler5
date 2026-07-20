// REQUIRES: cargo
// Differential end-to-end test for C unary minus on floating operands
// (FR-21 arithmetic coverage): the importer emits arith.negf for a
// float/double operand of unary '-', and convert-to-emitrust lowers it
// as `0.0 - x` (an emitrust.sub against a zero constant of the operand
// type), mirroring the `0 - x` spelling used for integer negation.
// Values are data-dependent (loop-carried accumulator, parameters) so
// no constant fold can hide the lowering. Everything prints with %f
// (six decimals); every value is an exact multiple of 0.25, so it is
// exactly representable and both languages print identical digits.
// --release is load-bearing: debug Rust panics on integer overflow
// where C wraps.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/float_negation > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

double flip(double x) { return -x; }

float flipf(float x) { return -x; }

int main(void) {
  double acc = 0.0;
  int i = 0;
  while (i < 4) {
    acc = acc + 0.25;
    double y = -acc;
    printf("y=%f\n", y);
    printf("p=%f\n", flip(y));
    i = i + 1;
  }
  float f = 2.5f;
  float g = -f;
  printf("g=%f\n", g);
  printf("h=%f\n", flipf(g));
  double m = -acc * 0.5 - -(acc - 0.25);
  printf("m=%f\n", m);
  if (-acc < 0.0) {
    printf("neg\n");
  }
  return 0;
}
