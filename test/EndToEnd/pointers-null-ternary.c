// REQUIRES: cargo
// CTS-P9 differential end-to-end test: pointer-typed conditional
// operators feeding the nullable-region machinery. The ternary condition
// flips across loop iterations, so the pointer's null/non-null state is
// a genuine runtime value: the null-check branches and the guarded
// dereferences all depend on it, and the final ternary's condition is
// computed from the mutated data. Every dereference is dominated by a
// null-check, so the program has no UB and the emitted guards never
// fire. Byte-identical stdout and exit codes against the clang-built
// native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name pointers_null_ternary_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/pointers_null_ternary_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int main(void) {
  int x = 5;
  int *q;
  int i;
  for (i = 0; i < 5; i++) {
    q = (i % 2) ? &x : 0;
    if (q) {
      *q = *q + i;
      printf("set x=%d\n", *q);
    } else {
      printf("null at %d\n", i);
    }
  }
  q = (x > 100) ? &x : 0;
  if (q == 0)
    printf("final null x=%d\n", x);
  else
    printf("final set x=%d\n", *q);
  q = (x < 100) ? &x : 0;
  if (q != 0)
    printf("second set x=%d\n", *q);
  else
    printf("second null x=%d\n", x);
  return 0;
}
