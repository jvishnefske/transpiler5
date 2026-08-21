// FR-99 frontier: what the Option-typed owned FAM return does NOT claim,
// pinned so nothing silently emits wrong code. FR-99 admits EXACTLY TWO
// call-site shapes for a nullable allocator — the guarded bind (the null test
// immediately following a declaration-with-initializer, folded to the Option
// discriminant) and the unguarded bind (unwrapped at the binding). Every
// other shape must keep a LOCATED diagnostic; in particular a null test that
// the guard recognition did not claim must NEVER fall through to the
// statically-non-null constant fold, which would silently delete the branch.
//   - Return-position frontier: a returned cursor into the callee's own owned
//     record, a returned parameter, and a mixed owned/borrowed return all keep
//     the verbatim historical returned-pointer wording (cJSON's
//     `cJSON_GetObjectItem` family lives here and stays out).
//   - Null-test frontier: a test that is not the adjacent guard, a guard with
//     an `else`, the `!= NULL` polarity, and a guard body that itself names
//     the bound local (which would read the payload the guard proved absent)
//     each reject at the test.
//   - Call-site frontier: a call used as an argument, dereferenced directly,
//     assigned to a pre-declared local, to a global, through a conditional
//     expression, in a `while` condition, or stored into an FR-96 Option
//     MEMBER — each keeps its own historical wording. The member arm is the
//     dangerous one: FR-96 admits only a DIRECT malloc into the member, and
//     FR-99 must not accidentally open it.
//   - An all-NULL "allocator" is still not an allocator (no owned return
//     site), and a call through a function POINTER cannot be classified at
//     all.
//
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/ret-cursor.c 2>&1 | FileCheck %s --check-prefix=RETCURSOR
// RUN: not emitrust-import-c %t/ret-param.c 2>&1 | FileCheck %s --check-prefix=RETPARAM
// RUN: not emitrust-import-c %t/ret-mixed.c 2>&1 | FileCheck %s --check-prefix=RETMIXED
// RUN: not emitrust-import-c %t/test-late.c 2>&1 | FileCheck %s --check-prefix=TESTLATE
// RUN: not emitrust-import-c %t/test-else.c 2>&1 | FileCheck %s --check-prefix=TESTELSE
// RUN: not emitrust-import-c %t/test-ne.c 2>&1 | FileCheck %s --check-prefix=TESTNE
// RUN: not emitrust-import-c %t/test-guard-uses.c 2>&1 | FileCheck %s --check-prefix=TESTUSES
// RUN: not emitrust-import-c %t/call-arg.c 2>&1 | FileCheck %s --check-prefix=CALLARG
// RUN: not emitrust-import-c %t/call-deref.c 2>&1 | FileCheck %s --check-prefix=CALLDEREF
// RUN: not emitrust-import-c %t/call-rebind.c 2>&1 | FileCheck %s --check-prefix=CALLREBIND
// RUN: not emitrust-import-c %t/call-global.c 2>&1 | FileCheck %s --check-prefix=CALLGLOBAL
// RUN: not emitrust-import-c %t/call-cond.c 2>&1 | FileCheck %s --check-prefix=CALLCOND
// RUN: not emitrust-import-c %t/call-while.c 2>&1 | FileCheck %s --check-prefix=CALLWHILE
// RUN: not emitrust-import-c %t/call-member.c 2>&1 | FileCheck %s --check-prefix=CALLMEMBER
// RUN: not emitrust-import-c %t/all-null.c 2>&1 | FileCheck %s --check-prefix=ALLNULL
// RUN: not emitrust-import-c %t/call-fnptr.c 2>&1 | FileCheck %s --check-prefix=CALLFNPTR

//--- ret-cursor.c
// A cursor into the callee's own owned record would dangle once the record
// drops at the end of the callee; the Option lift changes nothing here.
#include <stdlib.h>
typedef struct { unsigned short n; unsigned char buf[]; } bag;
static bag *bag_alloc(unsigned short n) {
  if (n == 0) return NULL;
  bag *b = malloc(sizeof(bag) + n);
  if (b == NULL) return NULL;
  b->n = n;
  return b;
}
unsigned char *first(unsigned short n) {
  bag *b = bag_alloc(n);
  if (b == NULL) return NULL;
  return b->buf;
}
// RETCURSOR: error: unsupported: returned pointer value (only a returned function address has a representation; a cursor into a callee-local region would dangle)

//--- ret-param.c
// A returned PARAMETER is a borrow of the caller's object, not an owned
// value; there is no owned-return representation for it.
#include <stdlib.h>
typedef struct { unsigned short n; unsigned char buf[]; } bag;
static bag *bag_alloc(unsigned short n) {
  if (n == 0) return NULL;
  bag *b = malloc(sizeof(bag) + n);
  if (b == NULL) return NULL;
  b->n = n;
  return b;
}
bag *pick(bag *x) { return x; }
unsigned use(unsigned short n) {
  bag *b = bag_alloc(n);
  if (b == NULL) return 0;
  return pick(b)->n;
}
// RETPARAM: error: unsupported: returned pointer value (only a returned function address has a representation; a cursor into a callee-local region would dangle)

//--- ret-mixed.c
// Mixed owned and borrowed returns: one site yields the caller's object and
// one yields a freshly owned record. No single return type represents both.
#include <stdlib.h>
typedef struct { unsigned short n; unsigned char buf[]; } bag;
bag *mixed(unsigned short n, bag *o) {
  if (n == 0) return NULL;
  if (o) return o;
  bag *b = malloc(sizeof(bag) + n);
  if (b == NULL) return NULL;
  b->n = n;
  return b;
}
unsigned use(unsigned short n) { return mixed(n, 0)->n; }
// RETMIXED: error: unsupported: returned pointer value (only a returned function address has a representation; a cursor into a callee-local region would dangle)

//--- test-late.c
// The null test is not the guard adjacent to the binding: the payload was
// already unwrapped, so there is no discriminant left to read.
#include <stdlib.h>
typedef struct { unsigned short n; unsigned char buf[]; } bag;
static bag *bag_alloc(unsigned short n) {
  if (n == 0) return NULL;
  bag *b = malloc(sizeof(bag) + n);
  if (b == NULL) return NULL;
  b->n = n;
  return b;
}
unsigned late(unsigned short n) {
  bag *b = bag_alloc(n);
  unsigned v = 0;
  if (b == NULL) return 0;
  v = b->n;
  free(b);
  return v;
}
// TESTLATE: error: unsupported: null test of 'b' outside its binding guard (a nullable flexible-array-record allocator result is unwrapped at the guard immediately following the binding)

//--- test-else.c
// A guard with an `else` would use the payload in the else arm, where the
// deferred unwrap has not run.
#include <stdlib.h>
typedef struct { unsigned short n; unsigned char buf[]; } bag;
static bag *bag_alloc(unsigned short n) {
  if (n == 0) return NULL;
  bag *b = malloc(sizeof(bag) + n);
  if (b == NULL) return NULL;
  b->n = n;
  return b;
}
unsigned with_else(unsigned short n) {
  bag *b = bag_alloc(n);
  if (b == NULL) {
    return 0;
  } else {
    unsigned v = b->n;
    free(b);
    return v;
  }
}
// TESTELSE: error: unsupported: null test of 'b' outside its binding guard (a nullable flexible-array-record allocator result is unwrapped at the guard immediately following the binding)

//--- test-ne.c
// The `!= NULL` polarity puts the payload use in the THEN arm, which the
// deferred unwrap does not dominate.
#include <stdlib.h>
typedef struct { unsigned short n; unsigned char buf[]; } bag;
static bag *bag_alloc(unsigned short n) {
  if (n == 0) return NULL;
  bag *b = malloc(sizeof(bag) + n);
  if (b == NULL) return NULL;
  b->n = n;
  return b;
}
unsigned positive(unsigned short n) {
  bag *b = bag_alloc(n);
  if (b != NULL) {
    unsigned v = b->n;
    free(b);
    return v;
  }
  return 0;
}
// TESTNE: error: unsupported: null test of 'b' outside its binding guard (a nullable flexible-array-record allocator result is unwrapped at the guard immediately following the binding)

//--- test-guard-uses.c
// The guard BODY names the bound local: reading the payload the guard just
// proved absent has no representation, so the shape is not a claimed guard.
#include <stdlib.h>
typedef struct { unsigned short n; unsigned char buf[]; } bag;
static bag *bag_alloc(unsigned short n) {
  if (n == 0) return NULL;
  bag *b = malloc(sizeof(bag) + n);
  if (b == NULL) return NULL;
  b->n = n;
  return b;
}
unsigned guard_uses(unsigned short n) {
  bag *b = bag_alloc(n);
  if (b == NULL) {
    free(b);
    return 0;
  }
  unsigned v = b->n;
  free(b);
  return v;
}
// TESTUSES: error: unsupported: null test of 'b' outside its binding guard (a nullable flexible-array-record allocator result is unwrapped at the guard immediately following the binding)

//--- call-arg.c
// The result used directly as a call argument is never bound to an owned
// local, so nothing claims it.
#include <stdlib.h>
typedef struct { unsigned short n; unsigned char buf[]; } bag;
static bag *bag_alloc(unsigned short n) {
  if (n == 0) return NULL;
  bag *b = malloc(sizeof(bag) + n);
  if (b == NULL) return NULL;
  b->n = n;
  return b;
}
static unsigned take(bag *b) { return b->n; }
unsigned use(unsigned short n) { return take(bag_alloc(n)); }
// CALLARG: error: unsupported pointer expression: CallExpr

//--- call-deref.c
// A direct dereference of the result skips the binding entirely.
#include <stdlib.h>
typedef struct { unsigned short n; unsigned char buf[]; } bag;
static bag *bag_alloc(unsigned short n) {
  if (n == 0) return NULL;
  bag *b = malloc(sizeof(bag) + n);
  if (b == NULL) return NULL;
  b->n = n;
  return b;
}
unsigned use(unsigned short n) { return bag_alloc(n)->n; }
// CALLDEREF: error: unsupported pointer expression: CallExpr

//--- call-rebind.c
// An assignment to a PRE-DECLARED local is not the declaration-bound shape
// the claim requires (the local's owned type is fixed at its declaration).
#include <stdlib.h>
typedef struct { unsigned short n; unsigned char buf[]; } bag;
static bag *bag_alloc(unsigned short n) {
  if (n == 0) return NULL;
  bag *b = malloc(sizeof(bag) + n);
  if (b == NULL) return NULL;
  b->n = n;
  return b;
}
unsigned use(unsigned short n) {
  bag *b;
  b = bag_alloc(n);
  return b->n;
}
// CALLREBIND: error: unsupported: pointer assigned a non-address value

//--- call-global.c
// A global cannot hold the owned record (globals have no owner model here).
#include <stdlib.h>
typedef struct { unsigned short n; unsigned char buf[]; } bag;
static bag *bag_alloc(unsigned short n) {
  if (n == 0) return NULL;
  bag *b = malloc(sizeof(bag) + n);
  if (b == NULL) return NULL;
  b->n = n;
  return b;
}
static bag *g;
unsigned use(unsigned short n) {
  g = bag_alloc(n);
  return g->n;
}
// CALLGLOBAL: error: unsupported: pointer assigned a non-address value

//--- call-cond.c
// A conditional expression is not a recognized allocation initializer.
#include <stdlib.h>
typedef struct { unsigned short n; unsigned char buf[]; } bag;
static bag *bag_alloc(unsigned short n) {
  if (n == 0) return NULL;
  bag *b = malloc(sizeof(bag) + n);
  if (b == NULL) return NULL;
  b->n = n;
  return b;
}
unsigned use(int c, unsigned short n) {
  bag *b = c ? bag_alloc(n) : bag_alloc(n + 1);
  return b->n;
}
// CALLCOND: error: unsupported: pointer assigned a non-address value

//--- call-while.c
// The retry loop rebinds the local each iteration; the Option temp would be
// moved more than once, so the shape is not claimed.
#include <stdlib.h>
typedef struct { unsigned short n; unsigned char buf[]; } bag;
static bag *bag_alloc(unsigned short n) {
  if (n == 0) return NULL;
  bag *b = malloc(sizeof(bag) + n);
  if (b == NULL) return NULL;
  b->n = n;
  return b;
}
unsigned use(unsigned short n) {
  bag *b;
  while ((b = bag_alloc(n)) != NULL) {
    return b->n;
  }
  return 0;
}
// CALLWHILE: error: unsupported: pointer assigned a non-address value

//--- call-member.c
// FR-96 admits a DIRECT malloc into an Option member and nothing else; a
// nullable allocator CALL stored there keeps the member's own rejection.
#include <stdlib.h>
struct hs_index { unsigned short size; short index[]; };
typedef struct { unsigned short n; struct hs_index *si; unsigned char buf[]; } enc;
static struct hs_index *idx_alloc(unsigned short n) {
  if (n == 0) return NULL;
  struct hs_index *x = malloc(sizeof(struct hs_index) + n * sizeof(short));
  if (x == NULL) return NULL;
  x->size = n;
  return x;
}
enc *enc_alloc(unsigned short n) {
  enc *e = malloc(sizeof(enc) + n);
  if (e == NULL) return NULL;
  e->n = n;
  e->si = idx_alloc(n);
  return e;
}
// CALLMEMBER: error: unsupported: pointer struct member 'si' is used outside the static-binding model

//--- all-null.c
// Every return site is NULL: there is no owned return at all, so the
// function is not an allocator and its result is not a record.
#include <stdlib.h>
typedef struct { unsigned short n; unsigned char buf[]; } bag;
static bag *never(unsigned short n) { (void)n; return NULL; }
unsigned use(unsigned short n) {
  bag *b = never(n);
  return b ? b->n : 0;
}
// ALLNULL: error: unsupported: dereference of an integer-carrier pointer

//--- call-fnptr.c
// Reached through a function pointer the callee is unknown at the call site,
// so no owned-return classification applies.
#include <stdlib.h>
typedef struct { unsigned short n; unsigned char buf[]; } bag;
static bag *bag_alloc(unsigned short n) {
  if (n == 0) return NULL;
  bag *b = malloc(sizeof(bag) + n);
  if (b == NULL) return NULL;
  b->n = n;
  return b;
}
typedef bag *(*mk)(unsigned short);
unsigned use(unsigned short n) {
  mk f = bag_alloc;
  bag *b = f(n);
  return b->n;
}
// CALLFNPTR: error: unsupported: function pointer result type
