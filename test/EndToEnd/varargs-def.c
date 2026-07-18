// REQUIRES: cargo
// CTS-P9 (00140): differential regression test for va_list-free variadic
// definitions imported as their fixed prototype. Covers the 00140 shape (a
// by-value struct, a struct pointer, and an int as named parameters, then
// trailing extras dropped at the call site: an int literal, the struct by
// value, and &s), statement-position and value-position calls, a nested
// call feeding a named argument, and the function result feeding both
// printf output and the process exit code. All dropped extras are
// side-effect-free. Byte-identical stdout and exit codes against the
// clang-built native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name varargs_def --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/varargs_def > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

struct pair {
  int a;
  int b;
};

int f1(struct pair v, struct pair *p, int n, ...) {
  if (v.a != p->a)
    return 0;
  return p->b + n;
}

int scale(int base, ...) {
  return base * 3 + 1;
}

int main(void) {
  struct pair s;
  s.a = 3;
  s.b = 4;

  // Result of a fixed-arity call feeds output.
  printf("r1=%d\n", f1(s, &s, 2));
  // Dropped extras: int literal, struct by value, &s.
  printf("r2=%d\n", f1(s, &s, 2, 1, s, &s));
  printf("r3=%d\n", scale(10));
  // A nested variadic-def call feeds the named argument; extras dropped.
  printf("r4=%d\n", scale(scale(2), 7, s));
  // Statement-position call with extras, the 00140 shape.
  f1(s, &s, 9, 1, s, &s);

  // The result feeds the exit code: both binaries must agree on 0.
  return f1(s, &s, 0) == 4 ? 0 : 1;
}
