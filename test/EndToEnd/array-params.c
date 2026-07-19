// REQUIRES: cargo
// C99-36: differential end-to-end test for array parameters with decay
// semantics. Every bracketed form — unsized `a[]`, sized `a[40]`, the
// C99 minimum-length `a[static 4]`, and the qualifier forms `a[const]`
// and `a[volatile]` (which qualify the decayed pointer itself and are
// erased with it) — decays to a pointer parameter and rides the ordinary
// slice / scalar-reference classification. The array stays above the
// owner-promotion element limit so the plain slice path is what runs.
// The crate build and the clang-linked native binary must produce
// byte-identical stdout.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/array_params > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int sum(int a[], int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s = s + a[i];
  return s;
}

int first(int a[40]) {
  return a[0];
}

int head4(int a[static 4]) {
  return a[0] + a[1] + a[2] + a[3];
}

int pick(int a[const], int i) {
  return a[i];
}

void bump(int a[static 1]) {
  *a = *a + 1;
}

void vbump(int a[volatile]) {
  *a = *a + 7;
}

void doubles(int a[], int n) {
  for (int i = 0; i < n; i++)
    a[i] = a[i] * 2;
}

int main(void) {
  int arr[40];
  for (int i = 0; i < 40; i++)
    arr[i] = i - 3;
  printf("sum=%d\n", sum(arr, 40));
  printf("first=%d\n", first(arr));
  printf("head4=%d\n", head4(arr));
  printf("pick=%d\n", pick(arr, 17));
  doubles(arr, 40);
  printf("sum2=%d\n", sum(arr, 40));
  printf("head4b=%d\n", head4(arr));
  int x = 5;
  bump(&x);
  vbump(&x);
  printf("x=%d\n", x);
  /* An interior pointer into the array feeds the decayed parameters too. */
  printf("tail=%d\n", sum(&arr[35], 5));
  printf("tail0=%d\n", first(&arr[35]));
  return 0;
}
