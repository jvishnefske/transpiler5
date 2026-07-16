// REQUIRES: cargo
// Phase-1a pointer decomposition, differential end-to-end test: transpile
// to a cargo crate, build it, and compare its stdout against the natively
// compiled C program. Exercises the 00004/00013/00016/00032/00037 shapes:
// a walking pointer summing an array, pointer difference arithmetic
// printed, dereferencing writes through &x, and pre/post increment
// interplay. main returns 0 and reports everything via printf, so lit's
// per-command exit-code checking covers both runs and diff covers the
// observable behavior. All cursor values stay in bounds; the program has
// no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/pointers_local > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int main(void) {
  int arr[5];
  for (int i = 0; i < 5; ++i) {
    arr[i] = i * 3 + 1;
  }

  /* Walking pointer summing the array. */
  int *p = &arr[0];
  int sum = 0;
  for (int i = 0; i < 5; ++i) {
    sum = sum + *p;
    p++;
  }
  printf("sum=%d\n", sum);

  /* Pointer difference arithmetic printed (ptrdiff_t is long). */
  int *lo = &arr[1];
  int *hi = &arr[4];
  long d = hi - lo;
  printf("d=%ld\n", d);
  printf("dd=%ld\n", &arr[3] - &arr[0]);

  /* Dereferencing writes through the address of a scalar. */
  int x = 7;
  int *px = &x;
  *px = 9;
  px[0] = px[0] + 2;
  printf("x=%d\n", x);

  /* Pre/post increment interplay on a walking pointer. */
  int *q = &arr[0];
  int a = *(q++);
  int b = *(++q);
  --q;
  int c = *q;
  q += 2;
  int e = *(q--);
  printf("a=%d b=%d c=%d e=%d\n", a, b, c, e);

  /* Same-object comparison drives a loop. */
  int *r = &arr[4];
  int below = 0;
  while (q < r) {
    below = below + 1;
    q++;
  }
  printf("below=%d\n", below);

  return 0;
}
