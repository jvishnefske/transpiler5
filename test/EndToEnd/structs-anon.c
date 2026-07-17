// REQUIRES: cargo
// CTS-R1 differential end-to-end test: bare anonymous struct types as a
// local, as an initialized file-scope global, nested inside a named struct,
// and with an anonymous-enum-typed member. Member reads and writes on every
// shape are reported via printf; main returns 0, so lit's per-command
// exit-code checking covers both runs and diff covers the observable
// behavior.
// --release is load-bearing: debug Rust panics on integer overflow where C
// wraps. The program below is deterministic, has no UB, and keeps all
// values comfortably small so no overflow can occur in either language.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/structs_anon > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

struct { int a; int b; int c; } g = {1, 2, 3};

struct outer {
  int x;
  struct {
    int y;
    int z;
  } nest;
};

struct {
  enum { X, Y = 7 } tag;
} labeled;

int main(void) {
  struct { int x; int y; } s;
  s.x = 3;
  s.y = 5;
  printf("s.x=%d s.y=%d diff=%d\n", s.x, s.y, s.y - s.x - 2);

  printf("g=%d,%d,%d sum=%d\n", g.a, g.b, g.c, g.a + g.b + g.c);
  g.b = g.b + 10;
  printf("g.b=%d\n", g.b);

  struct outer v;
  v.x = 1;
  v.nest.y = 2;
  v.nest.z = 3;
  printf("outer=%d nest=%d,%d total=%d\n", v.x, v.nest.y, v.nest.z,
         v.x + v.nest.y + v.nest.z);

  labeled.tag = Y;
  printf("tag=%d X=%d\n", labeled.tag, X);
  return 0;
}
