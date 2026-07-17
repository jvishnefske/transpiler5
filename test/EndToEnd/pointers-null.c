// REQUIRES: cargo
// CTS-P8 differential end-to-end test: NULL data-pointer constants under
// the Option-of-cursor model. The pointer's null/non-null state is
// data-dependent — it flips across loop iterations — so the discriminant
// is a genuine runtime value (a loop-carried bool after mem2reg), not a
// foldable constant: the null-check branches, the guarded dereference,
// and the final state all depend on it. Every dereference is dominated by
// a null-check, so the program has no UB and the emitted assert! guards
// never fire. Byte-identical stdout and exit codes against the
// clang-built native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name pointers_null_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/pointers_null_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int main(void) {
  int x = 40;
  int *p = 0;
  int i;
  for (i = 0; i < 3; i++) {
    if (p != 0) {
      *p = *p + 1;
      printf("p set: %d\n", *p);
    } else {
      printf("p null\n");
    }
    if (i == 0)
      p = &x;
    if (i == 1)
      p = 0;
  }
  if (p == 0)
    printf("final null\n");
  else
    printf("final set\n");
  printf("x=%d\n", x);
  return 0;
}
