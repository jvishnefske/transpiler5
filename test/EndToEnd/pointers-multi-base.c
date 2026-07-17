// REQUIRES: cargo
// CTS-P7 differential end-to-end test: a pointer ranging over several
// distinct objects (`p = &x; ... p = &y;`) under the enum-of-bases
// model. Which object each pointer designates is data-dependent at
// runtime (selected by loop parity and by values computed in earlier
// iterations), so the base-discriminant dispatch really executes both
// arms across the run. Exercises rebinding between two arrays with
// reads, writes, and cursor walks through both phases; two pointers over
// scalar objects with equality tests before and after a discriminant
// copy (the 00172 shape); and a loop-carried discriminant. main returns
// 0 and reports everything via printf, so lit's exit-code checking
// covers both runs and diff covers the observable behavior. Every
// dereference happens while the designated object is live and in
// bounds; the program has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/pointers_multi_base > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

/* Runtime-selected rebinding between two arrays, with writes and cursor
   walks through whichever array is active. */
void arrays_data_dependent(int n) {
  int evens[8];
  int odds[8];
  int *p;
  int i;
  for (i = 0; i < 8; i++) {
    evens[i] = 0;
    odds[i] = 0;
  }
  for (i = 0; i < n; i++) {
    if (i % 2 == 0) {
      p = evens;
    } else {
      p = odds;
    }
    p = p + (i % 8);
    *p = *p + i;
    p--;
    p++;
    *p = *p + 1;
  }
  for (i = 0; i < 8; i++) {
    printf("%d %d\n", evens[i], odds[i]);
  }
}

/* Two pointers over two scalars: equality discriminates which object
   each one designates (the 00172 shape), and the discriminant copies
   across p = q. */
void scalars_compare(int pick) {
  int a;
  int b;
  int *d;
  int *e;
  a = 12;
  b = 34;
  if (pick) {
    d = &a;
  } else {
    d = &b;
  }
  e = &b;
  printf("%d %d\n", *d, *e);
  printf("%d %d\n", d == e, d != e);
  d = e;
  printf("%d %d\n", d == e, d != e);
  *d = *d + 1;
  printf("%d %d\n", a, b);
}

/* The discriminant is loop-carried: the binding made in one iteration
   is dereferenced in the next. */
int carried(int n) {
  int a[4];
  int b[4];
  int *p;
  int i;
  for (i = 0; i < 4; i++) {
    a[i] = 0;
    b[i] = 0;
  }
  p = a;
  for (i = 0; i < n; i++) {
    *p = *p + 1;
    if (*p % 3 == 0) {
      p = b;
    } else {
      p = a;
    }
  }
  return a[0] * 100 + b[0];
}

int main(void) {
  arrays_data_dependent(13);
  scalars_compare(1);
  scalars_compare(0);
  printf("%d\n", carried(9));
  return 0;
}
