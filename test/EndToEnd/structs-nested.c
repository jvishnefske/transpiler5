// REQUIRES: cargo
// C99-42 pin, differential end-to-end test: nested structs (a named struct
// member inside another named struct), whole-struct assignment, and value
// (Copy) semantics. A whole-struct copy is taken, one copy is mutated
// through its nested member, and both copies are printed to prove the
// assignment copied rather than aliased. The same is exercised through a
// pointer deref (*p = s) and via a by-value parameter that the callee
// mutates without affecting the caller's struct. main returns 0 and
// reports via printf; diff covers the observable behavior. All values are
// small, deterministic, and overflow-free. No UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/structs_nested > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

struct Inner {
  int x;
  int y;
};

struct Outer {
  struct Inner inner;
  int tag;
};

void overwrite(struct Outer *p, struct Outer v) {
  *p = v;
}

int sum_by_value(struct Outer o) {
  o.inner.x = o.inner.x + 100;
  return o.inner.x + o.inner.y + o.tag;
}

int main(void) {
  struct Outer s2;
  s2.inner.x = 1;
  s2.inner.y = 2;
  s2.tag = 3;

  struct Outer s1;
  s1 = s2;
  s1.inner.x = 10;
  s1.tag = 30;
  printf("s1 %d %d %d\n", s1.inner.x, s1.inner.y, s1.tag);
  printf("s2 %d %d %d\n", s2.inner.x, s2.inner.y, s2.tag);

  struct Outer s3;
  s3.inner.x = 0;
  s3.inner.y = 0;
  s3.tag = 0;
  overwrite(&s3, s1);
  s3.inner.y = 20;
  printf("s3 %d %d %d\n", s3.inner.x, s3.inner.y, s3.tag);
  printf("s1 %d %d %d\n", s1.inner.x, s1.inner.y, s1.tag);

  int r = sum_by_value(s2);
  printf("r %d s2 %d %d %d\n", r, s2.inner.x, s2.inner.y, s2.tag);
  return 0;
}
