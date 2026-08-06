// REQUIRES: cargo
// FR-65 differential end-to-end test (the W4.5 heap memory-model change, Vec arm
// of the {array, Vec, span, Option} representation match): a runtime-sized heap
// buffer of a non-char scalar element type (`int *a = malloc(n * sizeof(int))`)
// used only through arbitrary `a[i]` writes/reads and `free` lifts WHOLE to an
// owned `let a: Vec<i32> = vec![0i32; n as usize]` — the malloc a real heap
// `Vec` allocation, `free` a no-op drop. The fill count derives from `argc` so
// it cannot fold at import time. The crate's stdout must byte-match the natively
// compiled C program for n > 0 AND n == 0 (the empty buffer, never indexed).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/malloc_vec_fill > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdio.h>
#include <stdlib.h>

// The intended runtime-sized heap-buffer idiom: malloc(n * sizeof(int)), an
// arbitrary per-element fill (Vec allows it — IndexMut), a read-back sum, a
// printf, and a free.
int sum_of_squares(unsigned int n) {
  int *a = malloc(n * sizeof(int));
  for (unsigned int i = 0; i < n; ++i)
    a[i] = (int)(i * i); // arbitrary fill: no constant-fill restriction
  int s = 0;
  for (unsigned int i = 0; i < n; ++i)
    s += a[i]; // read-back
  free(a);
  return s;
}

int main(int argc, char **argv) {
  printf("%d\n", sum_of_squares((unsigned int)argc));       // n = argc (>= 1)
  printf("%d\n", sum_of_squares((unsigned int)(argc * 5))); // a wider count
  printf("%d\n", sum_of_squares(0u));                       // n == 0: empty Vec
  return 0;
}
