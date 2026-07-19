// REQUIRES: cargo
// C99-18: differential end-to-end test for inline functions. The inline
// specifier is a no-op for the transpiler; every definition imports as an
// ordinary function. Covers a static inline helper (the task's required
// shape), an extern inline definition (the C99 spelling that provides the
// external definition), and a plain inline definition made linkable for
// the native build by a following extern declaration (C99 6.7.4p7). The
// crate build and the clang-linked native binary must produce
// byte-identical stdout.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/inline > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

static inline int scale(int x) {
  return x * 3;
}

extern inline int shift(int x) {
  return x + 40;
}

/* Plain C99 inline definition; the extern declaration below makes this
   TU provide the external definition, so the native link succeeds. */
inline int twice(int x) {
  return x + x;
}
extern int twice(int x);

static inline int chain(int x) {
  return scale(twice(x)) - shift(x);
}

int main(void) {
  printf("scale=%d\n", scale(7));
  printf("shift=%d\n", shift(2));
  printf("twice=%d\n", twice(21));
  printf("chain=%d\n", chain(5));
  int total = 0;
  for (int i = 0; i < 6; ++i)
    total = total + chain(i) + scale(i);
  printf("total=%d\n", total);
  return 0;
}
