// REQUIRES: cargo
// Stage-B regression: C copy-initialization of one struct object from another
// (`struct T y = x;`) is a whole-struct value copy, initialized from one
// loaded value exactly as the assignment form `y = x;` already was. The copy
// must be independent: mutating `y` after the copy must not affect `x`.
// Byte-matches the clang-native build; main returns 0 and reports via printf.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/struct_copy_init > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

struct T {
  int a;
  int b;
};

int main(void) {
  struct T x = {3, 4};
  struct T y = x; // whole-struct copy-initialization
  y.a += 10;      // independent of x
  struct T z = y; // chained copy
  printf("%d %d %d %d %d %d\n", x.a, x.b, y.a, y.b, z.a, z.b);
  return 0;
}
