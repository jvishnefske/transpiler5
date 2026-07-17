// REQUIRES: cargo
// CTS-R2 differential end-to-end test: anonymous struct/union members
// flattened into the parent — a global with a brace-elided initializer
// through an anonymous union member, a partially initialized global
// (zero-filled flattened tail), same-type union arms written through one
// spelling and read through another, an anonymous struct chain nested
// two levels, and a block-scope initializer list. Every observable value
// is printed and diffed against the native build; main returns 0, so
// lit's per-command exit-code checking covers both runs.
// --release is load-bearing: debug Rust panics on integer overflow where
// C wraps. The program below is deterministic, has no UB (every union
// read uses the same type as the last store, C99 6.5.2.3), and keeps all
// values small so no overflow can occur in either language.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/structs_anon_member > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

struct S1 {
  int a;
  int b;
};

struct S2 {
  int a;
  int b;
  union {
    int c;
    int d;
  };
  struct S1 s;
};

struct S2 g = {1, 2, 3, {4, 5}};
struct S2 part = {9};

typedef struct {
  int a;
  union {
    int b1;
    int b2;
  };
  struct { union { struct { int c; }; }; };
  struct {
    int d;
  };
} deep;

int main(void) {
  printf("g=%d,%d,%d,%d,%d,%d\n", g.a, g.b, g.c, g.d, g.s.a, g.s.b);
  g.d = g.d + 40;
  printf("g.c=%d g.d=%d\n", g.c, g.d);
  printf("part=%d,%d,%d,%d,%d\n", part.a, part.b, part.c, part.s.a,
         part.s.b);

  deep v;
  v.a = 1;
  v.b1 = 2;
  v.c = 3;
  v.d = 4;
  printf("deep=%d,%d,%d,%d,%d sum=%d\n", v.a, v.b1, v.b2, v.c, v.d,
         v.a + v.b2 + v.c + v.d);

  struct S2 l = {6, 7, 8, {9, 10}};
  printf("l=%d,%d,%d,%d,%d,%d\n", l.a, l.b, l.c, l.d, l.s.a, l.s.b);
  return 0;
}
