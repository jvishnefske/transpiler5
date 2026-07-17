// REQUIRES: cargo
// CTS-P5 second-order pointers, differential end-to-end test: transpile
// to a cargo crate, build it, and compare its stdout against the natively
// compiled C program. Exercises the 00005/00020 degenerate shape (`&p` of
// a scalar-bound pointer, `**pp` read and write) plus data-dependent
// indirection: the first-order cursor is re-pointed through `*pp` at an
// index computed at runtime, so the element `**pp` designates cannot be
// folded statically. main returns 0 and reports everything via printf, so
// lit's per-command exit-code checking covers both runs and diff covers
// the observable behavior. All cursor values stay in bounds; the program
// has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/pointers_ptr_to_ptr > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int main(void) {
  /* Degenerate 00005/00020 shape: scalar target, read and write. */
  int x = 0;
  int *p = &x;
  int **pp = &p;
  **pp = 7;
  printf("x=%d deref=%d\n", x, **pp);

  /* Data-dependent indirection: re-point p through *pp at a runtime
     index, then read and write through the double dereference. */
  int arr[5];
  for (int i = 0; i < 5; ++i) {
    arr[i] = i * 11;
  }
  int *q = &arr[0];
  int **qq = &q;
  int sum = 0;
  for (int i = 0; i < 5; ++i) {
    int idx = (i * 3) % 5; /* 0 3 1 4 2: every element, shuffled. */
    *qq = &arr[idx];
    sum = sum + **qq;
    **qq = **qq + i;
  }
  printf("sum=%d\n", sum);
  for (int i = 0; i < 5; ++i) {
    printf("arr[%d]=%d\n", i, arr[i]);
  }

  /* Rebinding qq to the same pointer variable stays supported. */
  qq = &q;
  *qq = &arr[3];
  printf("third=%d\n", **qq);
  return 0;
}
