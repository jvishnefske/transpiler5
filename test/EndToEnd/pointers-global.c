// REQUIRES: cargo
// CTS-P4 pointer-typed globals, differential end-to-end test: transpile
// to a cargo crate, build it, and compare its stdout against the natively
// compiled C program. Exercises data-dependent cursor updates across
// function calls on a global pointer into a global array (rebinding from
// a runtime index, ++ walks, += strides, reads and writes through the
// pointer, pointer difference against the array), a degenerate global
// pointer to a global scalar written through from another function, a
// pointer to a file-scope compound literal, and a calloc-promoted global
// pointer written and summed across calls (the 00040 shape). main returns
// 0 and reports everything via printf, so lit's per-command exit-code
// checking covers both runs and diff covers the observable behavior. All
// cursor values stay in bounds; the program has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/pointers_global > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdlib.h>

int printf(const char *, ...);

int x = 40;
int *p = &x;

int arr[6];
int *q = arr + 2;

struct Pair { int a; int b; };
struct Pair *pair = &(struct Pair){ 7, 9 };

int *t;

void bump(int amount) { *p = *p + amount; }

void rebind(int i) { q = &arr[i]; }

void advance(int stride) { q += stride; }

void put(int v) { *q = v; }

int get(void) { return *q; }

int offset(void) { return (int)(q - arr); }

int sumt(int n) {
  int s = 0;
  int i;
  for (i = 0; i < n; i++)
    s = s + t[i];
  return s;
}

int main(void) {
  int i;

  /* Degenerate global pointer to a global scalar, written across calls. */
  bump(2);
  printf("x=%d *p=%d\n", x, *p);

  /* Compound-literal base. */
  printf("pair=%d,%d\n", pair->a, pair->b);

  /* Cursor global: initializer offset, then data-dependent rebinds and
     walks across calls, with writes through the pointer. */
  for (i = 0; i < 6; i++)
    arr[i] = 10 * i;
  printf("init *q=%d off=%d\n", get(), offset());
  put(-5);
  printf("after put arr[2]=%d\n", arr[2]);
  rebind(5 % 4);          /* runtime index */
  printf("rebound *q=%d off=%d\n", get(), offset());
  q++;
  printf("walked *q=%d\n", *q);
  advance(-2);
  printf("strided *q=%d off=%d\n", get(), offset());
  for (i = 0; i < 3; i++) {
    *q = *q + i;
    q++;
  }
  q = arr;
  printf("final arr:");
  for (i = 0; i < 6; i++) {
    printf(" %d", *q);
    q++;
  }
  printf("\n");

  /* Calloc promotion: zeroed backing, element writes, sums across calls. */
  t = calloc(8, sizeof(int));
  printf("fresh sum=%d\n", sumt(8));
  for (i = 0; i < 8; i++)
    t[i] = i * i;
  printf("filled sum=%d t[3]=%d\n", sumt(8), t[3]);
  t[4]++;
  printf("bumped t[4]=%d\n", t[4]);

  return 0;
}
