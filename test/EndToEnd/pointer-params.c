// REQUIRES: cargo
// Phase-1b pointer parameters, differential end-to-end test: transpile to
// a cargo crate, build it, and compare its stdout against the natively
// compiled C program. Exercises the slice-parameter surface: a caller
// summing via a callee that walks a slice parameter, in-place doubling
// through a subscripted int* callee, a deref-only scalar out-parameter
// callee (unchanged Phase-1a shape), an interior-pointer call f(&arr[2], n),
// a decomposed pointer local passed onward, and a slice parameter resliced
// through another slice parameter. main returns 0 and reports everything
// via printf; diff covers the observable behavior. All cursors stay in
// bounds; the program has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/pointer_params > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

/* Callee that walks its slice parameter. */
int walk_sum(int *p, int n) {
  int s = 0;
  while (n > 0) {
    s = s + *p;
    p++;
    n--;
  }
  return s;
}

/* Callee that subscripts its slice parameter, writing in place. */
void double_all(int *a, int n) {
  for (int i = 0; i < n; i++) {
    a[i] = a[i] * 2;
  }
}

/* Deref-only scalar out-parameter (Phase-1a shape, unchanged). */
void bump(int *v) {
  *v = *v + 1;
}

/* Reslice pass-through: a slice parameter fed to another slice callee. */
int tail_sum(int *q, int n) {
  q++;
  return walk_sum(q, n - 1);
}

int main(void) {
  int arr[6];
  for (int i = 0; i < 6; i++) {
    arr[i] = i * 2 + 1;
  }

  /* Caller sums via the walking callee over the whole decayed array. */
  printf("sum=%d\n", walk_sum(arr, 6));

  /* Interior-pointer call: only the tail from index 2. */
  printf("tail=%d\n", walk_sum(&arr[2], 4));

  /* In-place doubling through the subscripted callee. */
  double_all(arr, 6);
  printf("doubled=%d\n", walk_sum(arr, 6));

  /* Scalar out-parameter callee. */
  int x = 41;
  bump(&x);
  printf("x=%d\n", x);

  /* Decomposed pointer local passed onward at its current cursor. */
  int *p = arr;
  p = p + 3;
  printf("fromp=%d\n", walk_sum(p, 3));

  /* Slice resliced through another slice parameter. */
  printf("tail2=%d\n", tail_sum(&arr[1], 5));

  /* Element borrow of a walked pointer fed to the scalar callee. */
  int *e = &arr[4];
  bump(e);
  printf("bumped=%d\n", arr[4]);

  return 0;
}
