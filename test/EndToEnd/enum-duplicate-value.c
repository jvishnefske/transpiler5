// REQUIRES: cargo
// Stage-C regression: a C enum with two enumerators sharing a value
// (`enum E { A = 1, B = 1 }`) is representable by the open-enum lowering --
// two associated consts of equal value (`const A: E = E(1); const B: E =
// E(1);`), valid Rust that preserves `A == B`. It used to reject as
// "unsupported: duplicate enumerator value". Byte-matches the clang-native
// build; main returns 0 and reports via printf.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/enum_duplicate_value > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

enum E { A = 1, B = 1, C = 2, D = 2, E_LAST };

int main(void) {
  enum E x = B;
  enum E y = D;
  printf("%d %d %d %d %d\n", (int)x, (int)y, x == A, C == D, (int)E_LAST);
  return 0;
}
