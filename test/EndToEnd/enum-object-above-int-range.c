// REQUIRES: cargo
// FR-169, the MINIMAL live reproduction of the enum relational miscompile.
//
// Nothing here is exotic: `enum M`'s two enumerators both fit `int`, there
// is no wide underlying type, no out-of-range enumerator, and no cast that
// C leaves undefined. Because no enumerator is negative, clang gives
// `enum M` an UNSIGNED underlying type, so an object of that type holding
// 3000000000 is exactly representable and `x > y` is defined to compare
// unsigned. Native prints `1 0 0`; before FR-169 the emitted crate printed
// `0 1 0`, because the importer emitted `struct M(u32)` for the storage and
// then compared `(x.0 as i32) > (y.0 as i32)`.
//
// This is the file that refutes the "unreachable" claim: the i32-range
// guard bounds ENUMERATOR values, not the values an enum-typed OBJECT may
// hold. It is kept separate from the wider matrix test so the one-line
// regression stays legible.
//
// The ternary keeps 3000000000 out of reach of constant folding: `argc`
// selects it, and both arms are `unsigned` so the conversion to `enum M` is
// value-preserving at every seed.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/enum_object_above_int_range > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/enum_object_above_int_range a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b > %t.n2.out && %t.crate/target/release/enum_object_above_int_range a b > %t.r2.out
// RUN: diff %t.n2.out %t.r2.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/enum_object_above_int_range a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out

int printf(const char *, ...);

enum M { M_A = 1, M_B = 2 };

int main(int argc, char **argv) {
  enum M x = (enum M)(argc > 100 ? 1u : 3000000000u);
  enum M y = M_A;
  printf("%d %d %d\n", x > y, x < y, x == y);
  return 0;
}
