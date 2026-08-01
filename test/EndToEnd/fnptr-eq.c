// REQUIRES: cargo
// Function-pointer equality between two non-null pointers (neither side is a
// null constant), exercising the None-aware `core::ptr::fn_addr_eq` lowering
// of `emitrust.cmp` on `Option<fn>` operands. Deterministic: same-function
// pointers compare equal, distinct-function pointers compare unequal, and the
// value is reported via printf so lit's byte-diff covers both runs.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/fnptr_eq > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }

typedef int (*binop)(int, int);

int main(void) {
  binop f = add;
  binop g = add;
  binop h = sub;
  printf("%d\n", f == g);
  printf("%d\n", f == h);
  printf("%d\n", f != h);
  printf("%d\n", f != g);
  return 0;
}
