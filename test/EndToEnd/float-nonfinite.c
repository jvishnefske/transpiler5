// REQUIRES: cargo
// FR-69: differential end-to-end test: non-finite float constants render
// byte-exactly via from_bits. The %f/%g shims print nan/-nan by SIGN BIT,
// so a NaN constant with the wrong sign is an observable byte divergence;
// f64::from_bits(0x<bits>) carries clang's exact folded sign+payload.
// Every non-finite value below is CONSTANT-FOLDED on purpose: a runtime
// 0.0/0.0 on x86 produces the negative "real indefinite" QNaN, which
// would test hardware NaN sign, not the constant path under test. The f32
// NaN routes through a float-returning function so it reaches the emitter
// as f32 (a cast at the use site would fold to the f64 NaN at import).
// Expected bytes (pinned by the clang native): "nan -nan inf -inf nan"
// under both %f and %g.
// --release is load-bearing: debug Rust panics on integer overflow where
// C wraps.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/float_nonfinite > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

float f_nan(void) { return 0.0f / 0.0f; }

int main(void) {
  double pn = 0.0 / 0.0;
  double nn = -(0.0 / 0.0);
  double pi = 1.0 / 0.0;
  double ni = -1.0 / 0.0;
  float fn = f_nan();
  printf("%f %f %f %f %f\n", pn, nn, pi, ni, (double)fn);
  printf("%g %g %g %g %g\n", pn, nn, pi, ni, (double)fn);
  return 0;
}
