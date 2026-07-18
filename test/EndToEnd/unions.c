// REQUIRES: cargo
// B1 differential end-to-end test: named unions modeled as one-field
// structs. Exercises the int/unsigned pun in both directions (store a
// negative int, print the unsigned arm; store a large unsigned, print the
// int arm), a union global with a constant initializer read through the
// other arm, a union nested in a struct, and a single-arm union. Every
// observable value is printed and diffed against the native build.
// No UB: reading a union member after storing another is defined (C11
// 6.5.2.3 with footnote 95 — the bytes are reinterpreted), and int/unsigned
// share width on this target, so the pun is bit-exact two's complement in
// both languages. --release is load-bearing: debug Rust panics on integer
// overflow where C wraps; the program itself performs no overflowing
// arithmetic, only representation puns.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/unions > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

union Pun {
  int i;
  unsigned int u;
};

union One {
  int only;
};

struct Holder {
  int tag;
  union Pun p;
};

union Pun g = {-7};

int main(void) {
  union Pun v;
  v.i = -1;
  printf("pun=%u\n", v.u);
  v.u = 3000000000u;
  printf("back=%d\n", v.i);

  printf("g=%d,%u\n", g.i, g.u);

  struct Holder h;
  h.tag = 3;
  h.p.i = -100;
  printf("holder=%d,%d,%u\n", h.tag, h.p.i, h.p.u);
  h.p.u = 2147483648u;
  printf("holder2=%d\n", h.p.i);

  union One s;
  s.only = 42;
  printf("one=%d\n", s.only);
  return 0;
}
