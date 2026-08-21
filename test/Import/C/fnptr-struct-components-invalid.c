// FR-102 frontier: the struct-pointer component admission covers EXACTLY
// a pointee that is a COMPLETE record, and every shape outside it keeps a
// LOCATED rejection — rejection is a feature, and nothing may silently
// emit wrong code. The pinned shapes, each probed against the built tool
// and each still rejecting VERBATIM after the admission landed:
//
//  - a `void *` component: deliberately OUT of FR-102's scope. A void*
//    has no pointee to classify from and no call-site consensus is
//    available for a component TYPE in a solo TU; it is the separately
//    ranked void-pointer front.
//  - a pointer-to-pointer component: no region and no record.
//  - a NESTED fn-ptr whose inner signature names a struct pointer: the
//    one-level `mappingFnPtrComponent` gate holds (classification is off
//    inside a nested component), so the inner pointer keeps the residual.
//  - an INCOMPLETE (forward-declared) pointee: the completeness test runs
//    BEFORE the dispatch precisely so this arm keeps the pointer-residual
//    wording instead of moving to `mapParamType`'s incomplete-struct one.
//  - a MUTUALLY RECURSIVE pair: `struct A` naming `struct B *` while
//    `struct B` is still incomplete is inherently the incomplete case, not
//    a bug to be fixed later.
//  - a struct-pointer RESULT type, which is a different arm entirely
//    and is untouched by this admission.
//  - the C++ path: the admission is gated to C.
//  - a NAMED-ENUM pointer component: clang calls an enum arithmetic, so
//    this never reaches the record arm at all — it takes FR-76's SLICE
//    branch and lands on the pre-existing slice-element rejection. This
//    pins why `ref`/`mut_ref` of an `enum` was NOT admitted to the dialect
//    component set: it is unreachable from the importer.
//  - a self-referential record with a self DATA pointer: owner promotion
//    turns such a record's struct-pointer parameter into an (owner
//    receiver, index) PAIR, which no fn_ptr component can ever carry.
//
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/void-component.c 2>&1 | FileCheck %s --check-prefix=VOIDCOMP
// RUN: not emitrust-import-c %t/ptr-to-ptr.c 2>&1 | FileCheck %s --check-prefix=PTRPTR
// RUN: not emitrust-import-c %t/nested-struct-ptr.c 2>&1 | FileCheck %s --check-prefix=NESTED
// RUN: not emitrust-import-c %t/incomplete.c 2>&1 | FileCheck %s --check-prefix=INCOMPLETE
// RUN: not emitrust-import-c %t/mutual.c 2>&1 | FileCheck %s --check-prefix=MUTUAL
// RUN: not emitrust-import-c %t/struct-result.c 2>&1 | FileCheck %s --check-prefix=RESULT
// RUN: not emitrust-import-c %t/probe.cpp 2>&1 | FileCheck %s --check-prefix=CPP
// RUN: not emitrust-import-c %t/enum-component.c 2>&1 | FileCheck %s --check-prefix=ENUMCOMP
// RUN: not emitrust-import-c %t/self-data.c 2>&1 | FileCheck %s --check-prefix=SELFDATA

//--- void-component.c
struct Ops { void (*g)(void *p, unsigned long n); };
int main(void) {
  struct Ops o;
  (void)o;
  return 0;
}
// VOIDCOMP: void-component.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer type outside a parameter position

//--- ptr-to-ptr.c
struct S { int x; };
struct Ops { void (*g)(struct S **p); };
int main(void) {
  struct Ops o;
  (void)o;
  return 0;
}
// PTRPTR: ptr-to-ptr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer type outside a parameter position

//--- nested-struct-ptr.c
struct S { int x; };
struct Ops { void (*k)(void (*inner)(struct S *s)); };
int main(void) {
  struct Ops o;
  (void)o;
  return 0;
}
// NESTED: nested-struct-ptr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer type outside a parameter position

//--- incomplete.c
struct Fwd;
struct Ops { void (*f)(struct Fwd *p); };
int main(void) {
  struct Ops o;
  (void)o;
  return 0;
}
// INCOMPLETE: incomplete.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer type outside a parameter position

//--- mutual.c
struct B;
struct A { int a; void (*fb)(struct B *b); };
struct B { int b; void (*fa)(struct A *a); };
int main(void) {
  struct A x;
  struct B y;
  (void)x;
  (void)y;
  return 0;
}
// MUTUAL: mutual.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer type outside a parameter position

//--- struct-result.c
struct S { int x; };
struct Ops { struct S *(*f)(int n); };
int main(void) {
  struct Ops o;
  (void)o;
  return 0;
}
// RESULT: struct-result.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: function pointer result type

//--- probe.cpp
struct S { int x; };
struct Ops { void (*f)(struct S *p); };
int main() {
  Ops o;
  (void)o;
  return 0;
}
// CPP: probe.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer type outside a parameter position

//--- enum-component.c
enum color { RED, GREEN };
struct Ops { void (*f)(enum color *c); };
int main(void) {
  struct Ops o;
  (void)o;
  return 0;
}
// ENUMCOMP: enum-component.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: slice parameter element type '!emitrust.enum<"color">'

//--- self-data.c
struct uf { struct uf *parent; int rank; int (*cb)(struct uf *n, int d); };
int main(void) {
  struct uf n;
  n.parent = 0;
  n.rank = 0;
  (void)n;
  return 0;
}
// SELFDATA: self-data.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: null pointer constant assigned to a pointer struct member
