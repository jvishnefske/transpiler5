// REQUIRES: cargo
// CTS-P2 pointer struct members, differential end-to-end test: transpile
// to a cargo crate, build it, and compare stdout against the natively
// compiled C program. Exercises the 00019/00049/00150 shapes plus
// data-dependent traffic through the static member bindings: a
// self-referential member chain, runtime values written and read through
// a member bound to a sibling local, a global struct's member dereferenced
// and written through, a member inside a pointer global's compound-literal
// backing, and a qualification-preserving cast feeding cursor arithmetic.
// The bindings are static but every value that flows through them is
// runtime data. main returns 0 and reports via printf; diff covers the
// observable behavior. No UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/pointers_member > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

struct S { struct S *p; int x; };

int gx = 10;
struct G { int a; int *gp; };
struct G g = { .gp = &gx, .a = 4 };

struct In { int a; int b; };
struct Out { struct In inner; struct In *pin; };
struct In gi = { 1, 2 };
struct Out *go = &(struct Out){ {3, 4}, &gi };

int chain(int seed) {
  struct S s;
  s.x = seed;
  s.p = &s;
  s.p->x = s.p->p->x + seed;
  return s.p->p->p->x;
}

int through_local(int n) {
  int t = 0;
  struct Q { int *ip; int pad; } q;
  q.ip = &t;
  int i;
  for (i = 1; i <= n; i++)
    *q.ip = *q.ip + i;
  return t;
}

int main(void) {
  printf("chain %d\n", chain(7));
  printf("local %d\n", through_local(5));

  printf("global %d %d\n", g.a, *g.gp);
  *g.gp = *g.gp + g.a;
  printf("global2 %d %d\n", *g.gp, gx);

  printf("compound %d %d %d\n", go->pin->a, go->pin->b, go->inner.b);
  go->pin->b = go->inner.b + *g.gp;
  printf("compound2 %d %d\n", gi.b, go->pin->b);

  int arr[3];
  int *p = (int *)arr;
  int k;
  for (k = 0; k < 3; k++)
    p[k] = k + *g.gp;
  int *q = (int *)(p + 1);
  printf("cast %d %d %d\n", arr[0], q[0], q[1]);
  return 0;
}
