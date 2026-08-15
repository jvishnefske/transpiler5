// REQUIRES: cargo
// FR-78 differential end-to-end test: a record containing a
// differing-aggregate-arm union (the lwIP `ip_addr` containment shape —
// `union { struct A; struct B; }` of differing sizes inside a struct).
// Before FR-78 the union's one-slot rejection cascaded through the whole
// record; now the union imports as OPAQUE STORAGE ([u8; 40] blob, marked
// struct_def) and the record is fully usable as long as the union is
// NEVER accessed through an arm. This test pins exactly that: the union
// travels as a blob through a whole-record copy, a by-value argument and
// return, a pointer parameter, and a zero-initialized global, while the
// SIBLING members are read, written, and printed. Seeds derive from argc
// so constant folding cannot hide a miscompile; every observable value is
// printed and byte-diffed against the clang-built native. No arm of the
// union is ever read or written (arm access is a located import
// rejection, pinned in test/Import/C/union-opaque-aggregate-invalid.c),
// so no UB and no blob byte is ever observable.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/union_opaque_aggregate > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native 7 8 > %t.native3.out
// RUN: %t.crate/target/release/union_opaque_aggregate 7 8 > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

int printf(const char *, ...);

struct A {
  int ax[10];
}; /* 40 bytes */

struct B {
  short bs;
  char bc;
}; /* 4 bytes */

union U {
  struct A a;
  struct B b;
}; /* differing aggregate arms: no one-slot model */

struct Rec {
  int before;
  union U u;
  int after;
};

struct Rec g_rec; /* zero-init global: the blob's Default covers it */

int sum_rec(struct Rec *r) { return r->before + r->after; }

struct Rec make_rec(int seed) {
  struct Rec r;
  r.before = seed;
  r.after = seed * 2 + 1;
  return r;
}

int take_by_value(struct Rec r) { return r.after - r.before; }

int main(int argc, char **argv) {
  struct Rec r = make_rec(argc + 41);
  struct Rec s = r; /* whole-record copy: the union travels as a blob */
  s.after += 5;
  printf("sum=%d\n", sum_rec(&r));
  printf("copy=%d,%d\n", s.before, s.after);
  printf("byval=%d\n", take_by_value(s));
  printf("global=%d\n", g_rec.before + g_rec.after + argc);
  return 0;
}
