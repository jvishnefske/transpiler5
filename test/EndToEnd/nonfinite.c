// REQUIRES: cargo
// FR-21/FR-25: differential end-to-end test: non-finite doubles printed with
// %f. The importer routes %f through the __emitrust_fmt_f64 helper so that
// infinities print as C's "inf"/"-inf" and NaN as "nan"/"-nan" (Rust's
// {:.6} alone would spell NaN as "NaN").
// The infinities below are produced at runtime from parameters, and the
// sign of an infinity from x/0.0 is fully determined by IEEE 754, so the
// two binaries must print identical text. A raw NaN is deliberately NOT
// diffed: the sign of the NaN produced by 0.0/0.0 is unspecified by C
// (Annex F) and observably differs between gcc (-nan) and clang/rustc
// (nan) on identical source, so the NaN spelling is asserted structurally
// in test/Import/C/printf.c instead.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/nonfinite > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

void show(double num, double den) {
  printf("q=%f\n", num / den);
}

int main(void) {
  double zero = 0.0;
  show(1.0, zero);   /* +inf */
  show(-2.5, zero);  /* -inf */
  show(-8.0, 0.5);   /* finite through the same helper */
  printf("done %f\n", 0.125);
  return 0;
}
