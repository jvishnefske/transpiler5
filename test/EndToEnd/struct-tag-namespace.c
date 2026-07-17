// REQUIRES: cargo
// CTS-R5: differential end-to-end test: a global `a` read and written while
// `struct a` values are built, passed by value, and mutated through a
// pointer — C's separate tag/ordinary namespaces landing in one Rust module
// (the tag renames to Struct_a; the global keeps its name). Also a
// collision-free `struct b` alongside, keeping its readable name. main
// returns 0 and reports everything via printf, so lit's per-command
// exit-code checking covers both runs and diff covers the observable
// behavior.
// --release is load-bearing: debug Rust panics on integer overflow where C
// wraps. The program below is deterministic, has no UB, keeps all values
// comfortably small, and puts every side-effecting call in its own
// statement so no unspecified evaluation order is exercised.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/struct_tag_namespace > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int a = 5;

struct a {
  int first;
  int second;
};

struct b {
  int scale;
};

int sum(struct a v) {
  return v.first + v.second;
}

void bump(struct a *p) {
  p->first = p->first + a;
  a = a + 1;
  p->second = p->second + a;
}

int main(void) {
  struct a x;
  x.first = a;
  a = a + 10;
  x.second = a;
  printf("%d\n", sum(x));
  printf("%d\n", a);

  bump(&x);
  printf("%d %d\n", x.first, x.second);

  struct b z;
  z.scale = 3;
  a = sum(x) * z.scale + a;
  printf("%d\n", a);
  return 0;
}
