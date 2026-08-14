// REQUIRES: cargo
// W2.13: by-value-capture lambda via lambda lifting, end to end. Builds a
// Rust crate through the full import + conversion + Rust-emission +
// `cargo build --release` pipeline and diffs its stdout against a
// `clang++ -std=c++17` build of the identical source, byte for byte.
// Based on corpus 00903's shape (a captured local, two calls), plus the
// semantics pin the lift MUST get right: a capture MUTATED between the
// lambda's creation and its calls (capture-by-value freezes the value at
// creation — the calls must not see the mutation), with the frozen value
// derived from argc so constant folding cannot hide a miscompile, and
// TWO lambdas alive in one function. A second run with one extra
// argument shifts the argc-derived capture, so the two legs must agree
// on two distinct input vectors.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stl_lambda > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native extra > %t.native.out
// RUN: %t.crate/target/release/stl_lambda extra > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

int main(int argc, char **argv) {
  // 00903's shape: one by-value capture, called twice. "34 42".
  int base = 30;
  auto add_base = [base](int x) { return base + x; };
  printf("%d %d\n", add_base(4), add_base(12));
  // The semantics pin: seed (argc-derived) and base are both MUTATED
  // between the lambda's creation and its calls; the calls must use the
  // frozen creation-point values (seed = argc * 7, base = 30).
  int seed = argc * 7;
  auto mul = [seed, base](int x) { return seed * x + base; };
  seed = 1000;
  base = 500;
  // Bare run (argc = 1): 7*2+30, 7*3+30 -> "44 51"; with one extra
  // argument (argc = 2): 14*2+30, 14*3+30 -> "58 72".
  printf("%d %d\n", mul(2), mul(3));
  // A parameter capture, and the mutated locals read AFTER the calls
  // (their post-mutation values must still be visible directly).
  auto pick = [argc](int x) { return argc + x; };
  printf("%d %d %d\n", pick(10), seed, base);
  return 0;
}
