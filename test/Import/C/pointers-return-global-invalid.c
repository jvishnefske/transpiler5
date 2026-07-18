// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/nullable.c 2>&1 | FileCheck %s --check-prefix=NULLABLE
// RUN: not emitrust-import-c %t/multi-base.c 2>&1 | FileCheck %s --check-prefix=MULTIBASE
// RUN: not emitrust-import-c %t/member-base.c 2>&1 | FileCheck %s --check-prefix=MEMBERBASE
// RUN: not emitrust-import-c %t/dangling.c 2>&1 | FileCheck %s --check-prefix=DANGLING

// CTS-S (00089) rejections: a global-pointer RETURN region is only
// representable when every return site yields the address of the SAME
// whole global (cursor 0) and no site returns NULL. Nullable returns and
// multi-base returns are located rejections, and returning the address
// of a callee-local object keeps the existing dangling rejection.

//--- nullable.c
// One path returns &gs, another returns NULL: the erased return cannot
// carry the discriminant.
struct S { int m; int n; };
struct S gs;
struct S *go(int c) {
  if (c)
    return &gs;
  return 0;
}
int main(void) { return go(1)->m; }
// NULLABLE: nullable.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: return sites mix a global address and NULL

//--- multi-base.c
// Return sites must agree on ONE whole-global base: &gs vs &gs2
// disagree.
struct S { int m; int n; };
struct S gs;
struct S gs2;
struct S *pick(int c) {
  if (c)
    return &gs;
  return &gs2;
}
int main(void) { return pick(1)->m; }
// MULTIBASE: multi-base.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: return sites disagree on the returned global base

//--- member-base.c
// Member addresses are not whole-global (cursor-0) bases: &gs.m vs
// &gs2.n disagree the same way ("&gs.member vs &gs2" itself does not
// type-check in C, so the member-address disagreement is pinned on the
// int* shape).
struct S { int m; int n; };
struct S gs;
struct S gs2;
int *pickm(int c) {
  if (c)
    return &gs.m;
  return &gs2.n;
}
int main(void) { return *pickm(1); }
// MEMBERBASE: member-base.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: return sites disagree on the returned global base

//--- dangling.c
// The same syntactic shape over a LOCAL struct stays the existing
// dangling rejection (existing wording, reused).
struct S { int m; int n; };
struct S *bad(void) {
  struct S s;
  s.m = 1;
  return &s;
}
int main(void) { return bad()->m; }
// DANGLING: dangling.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: returned pointer value
