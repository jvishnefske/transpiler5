// REQUIRES: cargo
// C99-7: differential end-to-end test for type qualifiers on the
// accepted side. const globals (scalar and array) become immutable Rust
// statics, a `static const` local becomes a mangled const module
// global, const locals (scalar and aggregate) keep their ordinary
// lowering, a const pointee parameter classifies like its unqualified
// spelling (mut_ref or slice per the region analysis), restrict is
// accepted and ignored, and qualification-only pointer casts (adding
// and dropping const) are transparent. Everything is reported via
// printf so diff covers the observable behavior. --release matches the
// C wrap-on-overflow semantics; values here stay small anyway.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/qualifiers > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

const int SCALE = 3;
const short STEPS[4] = {1, 2, 3, 4};

int read_const(const int *p) { return *p; }

int scale_by(const int factor) { return factor * SCALE; }

int deref_restrict(int *restrict p) { return *p + 1; }

int sum_restrict(const int *restrict a, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s = s + a[i];
  return s;
}

int cast_qualifiers(void) {
  int x = 9;
  const int *cp = (const int *)&x;  /* add const */
  int *mp = (int *)cp;              /* drop const */
  *mp = 11;
  return *cp;
}

int string_arg(void) {
  const char *s = "quals";
  int n = 0;
  while (*s) {
    n++;
    s++;
  }
  return n;
}

int main(void) {
  const int local = 5;
  const int arr[2] = {10, 20};
  static const int cached = 40;

  printf("read_const %d\n", read_const(&local));
  printf("arr %d %d\n", arr[0], arr[1]);
  printf("scale_by %d\n", scale_by(7));
  int y = 6;
  printf("deref_restrict %d\n", deref_restrict(&y));
  int buf[3] = {7, 8, 9};
  printf("sum_restrict %d\n", sum_restrict(buf, 3));
  printf("cast_qualifiers %d\n", cast_qualifiers());
  printf("string_arg %d\n", string_arg());
  printf("globals %d %d\n", SCALE, cached);
  int t = 0;
  for (int i = 0; i < 4; i++)
    t = t + STEPS[i];
  printf("steps %d\n", t);
  return 0;
}
