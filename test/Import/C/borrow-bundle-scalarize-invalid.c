// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/selfref.c 2>&1 | FileCheck %s --check-prefix=SELFREF
// RUN: not emitrust-import-c %t/escape.c 2>&1 | FileCheck %s --check-prefix=ESCAPE
// RUN: not emitrust-import-c %t/addrlocal.c 2>&1 | FileCheck %s --check-prefix=ADDRLOCAL
// RUN: not emitrust-import-c %t/copy.c 2>&1 | FileCheck %s --check-prefix=BYVAL
// RUN: not emitrust-import-c %t/arraymem.c 2>&1 | FileCheck %s --check-prefix=ARRAYMEM
// RUN: not emitrust-import-c %t/voidmem.c 2>&1 | FileCheck %s --check-prefix=VOIDMEM
// RUN: not emitrust-import-c %t/structmem.c 2>&1 | FileCheck %s --check-prefix=STRUCTMEM
// RUN: not emitrust-import-c %t/store.c 2>&1 | FileCheck %s --check-prefix=STORE
// RUN: not emitrust-import-c %t/noinstance.c 2>&1 | FileCheck %s --check-prefix=NOINSTANCE
// RUN: not emitrust-import-c %t/subscript.c 2>&1 | FileCheck %s --check-prefix=SUBSCRIPT
// RUN: not emitrust-import-c %t/nulltest.c 2>&1 | FileCheck %s --check-prefix=NULLTEST
// RUN: not emitrust-import-c %t/reassign.c 2>&1 | FileCheck %s --check-prefix=REASSIGN

// FR-101 frontier. Borrow-bundle scalarization (borrow-bundle-
// scalarize.c) is a WHOLE-TU recognition: the transform fires only when
// EVERY clause of the gate holds for the record type, TU-wide, and the
// consequence of any clause failing is that the record keeps today's
// emission and today's LOCATED rejections. Rejection is a feature here
// in a sharper-than-usual sense: a bundle rewrite that fired on a shape
// it does not model would silently move a caller-visible store, or
// silently reborrow an empty slice, and nothing downstream could see
// it. Every wording below is pinned verbatim as measured against the
// built tool.
//
// Two of these arms are the gate clauses the FR entry's prose did NOT
// name and the spike measured into it — STORE and NOINSTANCE. Both pass
// every clause design.md wrote down, and both must keep rejecting.

// SELF-REFERENTIAL: a member of type `B *` (the linked-list / union-find
// / malloc-pool node family). Scalarizing a node type would have to
// scalarize its own pointer member, which has no bound source; the
// clause excludes the whole family by construction.
// SELFREF: selfref.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer parameter used outside a direct dereference

//--- selfref.c
#include <stddef.h>
#include <stdint.h>
typedef struct node { struct node *next; uint8_t *buf; size_t *n; } node;
static void use(node *p) { *p->n += 1; }
int main(void) {
  node b; uint8_t d[4]; size_t k = 0;
  b.next = 0; b.buf = d; b.n = &k;
  use(&b);
  return (int)k;
}

// ESCAPING to a GLOBAL: the instance's address is taken somewhere other
// than a `B *` call-argument position, so the members it carries would
// outlive the substitution the transform performs.
// ESCAPE: escape.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global pointer bound to local object 'b' (the borrow would outlive the object)

//--- escape.c
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *buf; size_t *n; } bundle;
static bundle *g;
static void use(bundle *p) { *p->n += 1; }
int main(void) {
  bundle b; uint8_t d[4]; size_t k = 0;
  b.buf = d; b.n = &k;
  g = &b;
  use(&b);
  return (int)k;
}

// The same clause with a LOCAL pointer instead of a global: `&b` is
// consumed by an assignment, not by a call argument, so there is no
// argument position for the expansion to happen at.
// ADDRLOCAL: addrlocal.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer parameter used outside a direct dereference

//--- addrlocal.c
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *buf; size_t *n; } bundle;
static void use(bundle *p) { *p->n += 1; }
int main(void) {
  bundle b; bundle *keep; uint8_t d[4]; size_t k = 0;
  b.buf = d; b.n = &k;
  keep = &b;
  use(keep);
  return (int)k;
}

// WHOLESALE by-value copy: the same instance is passed BY VALUE and by address in
// one call. A by-value B has no scalarized spelling, and the copy is a
// second, independent instance the substitution cannot track. (This is
// also one of the two clauses that keeps c-testsuite 00140 — a live
// ledger entry with exactly this shape — on its current path.)
// BYVAL: copy.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer parameter used outside a direct dereference

//--- copy.c
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *buf; size_t *n; } bundle;
static void take(bundle v, bundle *p) { *p->n += v.buf[0]; }
int main(void) {
  bundle b; uint8_t d[4]; size_t k = 0;
  d[0] = 1; b.buf = d; b.n = &k;
  take(b, &b);
  return (int)k;
}

// An ARRAY member: not an arithmetic scalar and not a data pointer, so
// there is no per-member parameter type to expand it to.
// ARRAYMEM: arraymem.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer parameter used outside a direct dereference

//--- arraymem.c
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t pad[4]; size_t *n; } bundle;
static void use(bundle *p) { *p->n += p->pad[0]; }
int main(void) {
  bundle b; size_t k = 0;
  b.pad[0] = 3; b.n = &k;
  use(&b);
  return (int)k;
}

// A `void *` member: no pointee to classify from, so no parameter type.
// This is the same clause that keeps void-param-invalid.c's FIELD arm
// (`struct holder { void *p; }`) on its current rejection.
// VOIDMEM: voidmem.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer parameter used outside a direct dereference

//--- voidmem.c
#include <stddef.h>
#include <stdint.h>
typedef struct { void *raw; size_t *n; } bundle;
static void use(bundle *p) { *p->n += (p->raw != 0); }
int main(void) {
  bundle b; uint8_t d[4]; size_t k = 0;
  b.raw = d; b.n = &k;
  use(&b);
  return (int)k;
}

// A STRUCT-POINTER member: the pointee is not arithmetic, so the member
// has no scalar-parameter spelling either. Same clause as
// multi-tu-external-requirement-const-struct-negative.c's ADDR-STORE
// arm (`struct Holder { const struct S *ptr; }`).
// STRUCTMEM: structmem.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer parameter used outside a direct dereference

//--- structmem.c
#include <stddef.h>
#include <stdint.h>
struct inner { int v; };
typedef struct { struct inner *ip; size_t *n; } bundle;
static void use(bundle *p) { *p->n += (size_t)p->ip->v; }
int main(void) {
  bundle b; struct inner s; size_t k = 0;
  s.v = 2; b.ip = &s; b.n = &k;
  use(&b);
  return (int)k;
}

// A member STORE through a `B *` parameter (`p->buf = q`). This clause
// is NOT in the FR entry's prose: `p->buf` is syntactically the
// admitted `p->member` projection, so the shape passes every clause
// design.md wrote down. It must still reject, because scalarization
// turns a caller-VISIBLE store into a store to a by-value copy of the
// caller's argument — the member write would be silently lost. Two
// currently-pinned rejections (pointers-member-array-local-invalid.c's
// ESCAPE arm and pointers-member-invalid.c's aliased-write arm) have
// exactly this shape and must not move.
// STORE: store.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member 'buf' is used outside the static-binding model
// STORE: store.c:{{[0-9]+}}:{{[0-9]+}}: note: first unresolvable use is here

//--- store.c
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *buf; size_t *n; } bundle;
static void use(bundle *p, uint8_t *q) { p->buf = q; *p->n += 1; }
int main(void) {
  bundle b; uint8_t d[4]; uint8_t e[4]; size_t k = 0;
  b.buf = d; b.n = &k;
  use(&b, e);
  return (int)k;
}

// A bundle-shaped struct that is BUILT but never PASSED: the confining
// clause requires a `B *` PARAMETER to exist before anything is
// rewritten, so this shape — the bundle idiom minus the call — keeps
// its pre-FR-101 rejection verbatim. This is the deliberate cost of the
// clause that makes the whole transform additive, and it is pinned here
// so that a later widening of the gate has to move a pin.
// NOINSTANCE: noinstance.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member assigned a non-address value

//--- noinstance.c
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *buf; size_t *n; } bundle;
static void build(uint8_t *out, size_t *n) {
  bundle oi;
  oi.buf = out;
  oi.n = n;
  (void)oi;
}
int main(void) {
  uint8_t d[4]; size_t k = 0;
  build(d, &k);
  return (int)k;
}

// A SUBSCRIPTED `B *` parameter: `p[0]` names an element of a run, and
// a scalarized parameter list carries no run.
// SUBSCRIPT: subscript.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member 'n' of an unresolvable struct instance

//--- subscript.c
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *buf; size_t *n; } bundle;
static void use(bundle *p) { *p[0].n += 1; }
int main(void) {
  bundle b; uint8_t d[4]; size_t k = 0;
  b.buf = d; b.n = &k;
  use(&b);
  return (int)k;
}

// A NULL-TESTED `B *` parameter: the scalarized form has no single
// value to test, and an expanded parameter list cannot be "null".
// NULLTEST: nulltest.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member 'n' of an unresolvable struct instance

//--- nulltest.c
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *buf; size_t *n; } bundle;
static void use(bundle *p) { if (p == 0) { return; } *p->n += 1; }
int main(void) {
  bundle b; uint8_t d[4]; size_t k = 0;
  b.buf = d; b.n = &k;
  use(&b);
  return (int)k;
}

// A REASSIGNED `B *` parameter: after `p = q` the projections of `p`
// name a different instance's members, which no static expansion can
// express.
// REASSIGN: reassign.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assignment would rebind to a different object

//--- reassign.c
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *buf; size_t *n; } bundle;
static void use(bundle *p, bundle *q) { p = q; *p->n += 1; }
int main(void) {
  bundle b, c; uint8_t d[4]; size_t k = 0;
  b.buf = d; b.n = &k; c.buf = d; c.n = &k;
  use(&b, &c);
  return (int)k;
}
