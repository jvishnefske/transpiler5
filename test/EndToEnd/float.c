// REQUIRES: cargo
// FR-21: differential end-to-end test: double arithmetic printed with %f (which
// the importer maps to Rust's {:.6}, matching C's default six decimals).
// main returns 0 and reports everything via printf, so lit's per-command
// exit-code checking covers both runs and diff covers the observable
// behavior.
// --release is load-bearing: debug Rust panics on integer overflow where C
// wraps. The program below is deterministic and has no UB; every double
// value is an exact multiple of 0.25/0.5, so it is exactly representable
// and both languages print identical digits.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/float > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

double mix(double a, double b) {
  return a * 0.5 + b * 0.25;
}

int main(void) {
  double step = 0.25;
  double acc = 0.0;
  int i = 0;
  while (i < 4) {
    acc = acc + step;
    printf("acc=%f\n", acc);
    i = i + 1;
  }
  double m = mix(acc, 8.5);
  printf("m=%f\n", m);
  double d = mix(m, m) - 0.5;
  printf("d=%f\n", d);
  return 0;
}
