// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/walk-past-member.c 2>&1 | FileCheck %s --check-prefix=WALK
// RUN: not emitrust-import-c %t/union-member-addr.c 2>&1 | FileCheck %s --check-prefix=UNION

// CTS-P9 boundaries of the member-path pointer base: a scalar member is
// a degenerate one-element run, and union storage has no unaliased
// member place to root a region at. Each file pins one boundary with a
// located rejection.

// A scalar member base has no element run behind it: any pointer
// arithmetic walks past the member into sibling storage, which the
// member-path binding cannot represent.
// WALK: walk-past-member.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer arithmetic on the address of a struct member

//--- walk-past-member.c
struct S { int a; int b; };
int main(void) {
  struct S s;
  int *p;
  s.a = 1;
  s.b = 2;
  p = &s.a;
  p = p + 1;
  return *p;
}

// An anonymous-union arm aliases its sibling arms in one storage slot;
// a member-path base rooted there could not keep the aliased names
// coherent, so taking the address of a union member stays rejected.
// UNION: union-member-addr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: taking the address of a union member

//--- union-member-addr.c
struct H {
  int head;
  union {
    int a;
    int b;
  };
  int tail;
};
int main(void) {
  struct H h;
  int *p = &h.b;
  h.a = 3;
  return *p;
}
