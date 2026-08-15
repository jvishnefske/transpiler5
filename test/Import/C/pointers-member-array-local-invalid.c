// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/escape.c 2>&1 | FileCheck %s --check-prefix=ESCAPE
// RUN: not emitrust-import-c %t/cross-root.c 2>&1 | FileCheck %s --check-prefix=CROSSROOT
// RUN: not emitrust-import-c %t/sibling-fields.c 2>&1 | FileCheck %s --check-prefix=SIBLING
// RUN: not emitrust-import-c %t/nested-chain.c 2>&1 | FileCheck %s --check-prefix=NESTED
// RUN: not emitrust-import-c %t/global-root.c 2>&1 | FileCheck %s --check-prefix=GLOBAL
// RUN: not emitrust-import-c %t/member-row.c 2>&1 | FileCheck %s --check-prefix=ROW
// RUN: not emitrust-import-c %t/scalar-member-arith.c 2>&1 | FileCheck %s --check-prefix=SCALARITH
// RUN: not emitrust-import-c %t/two-multibase-args.c 2>&1 | FileCheck %s --check-prefix=TWOMULTI
// RUN: not emitrust-import-c %t/multibase-result.c 2>&1 | FileCheck %s --check-prefix=RESULT
// RUN: emitrust-import-c %t/addr-of-local.c | FileCheck %s --check-prefix=ADDROF

// FR-93 frontier: member-array-backed pointer LOCALS are admitted only
// for the provable shapes — a SINGLE-link typed member decay over a
// directly named local struct or struct-pointer parameter, a
// byte-region window chain over a local root, and the multi-base cbc
// call shape with EXACTLY ONE multi-base slice argument per void call.
// Everything else keeps a located rejection, pinned verbatim as
// measured, because each shape would otherwise emit wrong code or fail
// only downstream: an ESCAPING store of a cursor local into a struct
// field has no static binding; a reassignment across two DIFFERENT
// member roots (and across sibling fields of one root) would need a
// per-base cursor coordinate the shared cursor cell cannot carry for
// member places; a NESTED typed chain and a GLOBAL root have no
// admitted backing; a member ROW of a 2D member array would need a
// row-scaled member cursor; scalar-member arithmetic would walk past
// the member into sibling storage; more than one multi-base argument
// per call would multiply dispatch arms; and a multi-base argument to
// a RESULT-carrying callee has no staged result representation this
// wave. Rejection is a feature: every wording below is measured.

// An escaping store of a member-array-backed cursor local into a
// pointer struct member: outside the static-binding model, exactly as
// for a top-level-array cursor local.
// ESCAPE: escape.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member 'save' is used outside the static-binding model

//--- escape.c
struct S { int arr[4]; };
struct H { int *save; };
static void f(struct S *s, struct H *h) {
  int *p = s->arr;
  p += 1;
  h->save = p;
}
int main(void) { struct S s; struct H h; f(&s, &h); return 0; }

// Reassignment across DIFFERENT member roots: two member-array bases
// in one region have no shared cursor coordinate; the join is named
// and rejected at the local's declaration.
// CROSSROOT: error: unsupported: pointer 'p' would join objects 's' and 't' into one region

//--- cross-root.c
struct S { int arr[4]; };
static int f(struct S *s, struct S *t) {
  int *p = s->arr;
  p = t->arr;
  return p[0];
}
int main(void) { struct S a; struct S b; return f(&a, &b); }

// Sibling member arrays of ONE root: still two member-place backings
// with distinct cursor coordinates; same join rejection.
// SIBLING: error: unsupported: pointer 'p' would join objects 's' and 's' into one region

//--- sibling-fields.c
struct S { int arr[4]; int brr[4]; };
static int f(struct S *s) {
  int *p = s->arr;
  p = s->brr;
  return p[0];
}
int main(void) { struct S s; return f(&s); }

// A NESTED typed chain (`s->in.arr`): only single-link member decays
// are admitted this wave; the historical rejection stays verbatim.
// NESTED: nested-chain.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- nested-chain.c
struct Inner { int arr[4]; };
struct S { struct Inner in; };
static int f(struct S *s) {
  int *p = s->in.arr;
  return p[0];
}
int main(void) { struct S s; return f(&s); }

// A GLOBAL struct's member array: the member place is a staged local
// copy, so a cursor local over it would silently lose writes; the
// chain root must have local storage (the FR-74 rule, unchanged).
// GLOBAL: global-root.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- global-root.c
struct S { int arr[4]; };
static struct S g;
int main(void) {
  int *p = g.arr;
  return p[0];
}

// A decayed ROW of a 2D member array (`s->mat[i]`): the member cursor
// would need row scaling; the historical rejection stays verbatim.
// ROW: member-row.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- member-row.c
struct S { int mat[3][4]; };
static int f(struct S *s, int i) {
  int *p = s->mat[i];
  return p[0];
}
int main(void) { struct S s; return f(&s, 1); }

// Arithmetic on a SCALAR member address stays rejected: the walk would
// leave the member's one-element run into sibling storage. (The arrow
// form `&s->x` has no analysis classification at all and keeps the
// non-address wording; this pins the dot form's dedicated wording.)
// SCALARITH: scalar-member-arith.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer arithmetic on the address of a struct member

//--- scalar-member-arith.c
struct S { int x; int y; };
int main(void) {
  struct S s;
  int *p = &s.x;
  p += 1;
  return *p;
}

// TWO multi-base pointer arguments in one call would multiply the
// dispatch arms; the historical multi-base argument rejection fires.
// TWOMULTI: two-multibase-args.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: passing a pointer bound to multiple objects to a function

//--- two-multibase-args.c
typedef unsigned char u8;
static void two(u8 *x, const u8 *y) { x[0] = (u8)(x[0] + y[0]); }
static void f(u8 *a, u8 *b, int k) {
  u8 *p = a;
  u8 *q = a;
  if (k) { p = b; q = b; }
  two(p, q);
}
int main(void) { u8 a[4] = {1, 2, 3, 4}; u8 b[4] = {5, 6, 7, 8}; f(a, b, 1); return (int)a[0]; }

// A multi-base argument to a result-carrying callee: the whole-call
// dispatch is void-only this wave (no staged result representation).
// RESULT: multibase-result.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: passing a pointer bound to multiple objects to a function

//--- multibase-result.c
typedef unsigned char u8;
static u8 head(const u8 *x) { return x[0]; }
static u8 f(u8 *a, u8 *b, int k) {
  const u8 *p = a;
  if (k)
    p = b;
  return head(p);
}
int main(void) { u8 a[4] = {1, 2, 3, 4}; u8 b[4] = {5, 6, 7, 8}; return (int)f(a, b, 1); }

// Address-of a member-array-backed local (`&p` through a
// pointer-to-pointer): the second-order selection is static and the
// deref composes over the member backing — the write lands on the
// member place, never silently dropped. Pinned as a COMPOSITION (the
// program imports; the store reaches s.arr[1]'s place).
// ADDROF: func.func @f
// ADDROF: emitrust.member %{{.*}}["arr"]
// ADDROF: emitrust.assign

//--- addr-of-local.c
struct S { int arr[4]; };
static int f(struct S *s) {
  int *p = s->arr;
  int **q = &p;
  p += 1;
  **q = 9;
  return s->arr[1];
}
int main(void) { struct S s; s.arr[1] = 0; return f(&s); }
