// FR-94 frontier: what the owned-tail admission does NOT claim, pinned so
// nothing silently emits wrong code.
//   - A whole-record assignment of an admitted record is a LOCATED rejection:
//     C's FAM assignment copies the fields and NOT the tail, while any Rust
//     rendering of the owned-Vec representation either moves or clones the
//     tail — no spelling reproduces C, so the shape must not import.
//   - A PADDING-GAP layout (offsetof(tail) != sizeof) is outside the
//     admission: C legally indexes the tail below sizeof there, which the
//     tail-only Vec cannot represent. Its accesses keep the historical FAM
//     wording and its allocations get the dedicated FAM-alloc rejection.
//   - FR-95 FLIPPED the non-u8 arm: a Vec-mappable typed FAM leaf
//     (hs_index's `int16_t index[]`) is now ADMITTED, so the frontier moves
//     to the typed-tail COUNT ARITHMETIC and VIEWS:
//     - a multiply factor that does not fold to sizeof(elem) (`n * 3`), and
//       FR-94's plain-add byte form over a typed tail (`sizeof(S) + n` — no
//       compile-time division), keep the dedicated FAM-alloc rejection;
//     - a typed tail behind a gap layout keeps the same alloc rejection
//       (the gap gate is element-independent);
//     - a mismatched-element or void view of a typed tail rejects at the
//       binding (the void wildcard is byte-granular and stays u8-only), and
//       a byte-view CAST of a typed tail keeps the non-address rejection;
//     - an element with no Vec mapping (long double) stays entirely on the
//       historical rejections (alloc AND access wordings).
//   - A FAM record reached through an extern pointer has no recognized
//     allocation; the access stays located.
//   - The malloc RE-BINDING form (`d = malloc(...)` after the declaration)
//     is not the declaration-bound owned-tail shape; it gets the dedicated
//     FAM-alloc rejection instead of the old over-counted struct-array
//     backing (which mis-sized the allocation as tail-bytes/sizeof extra
//     whole records).
//   - `free` of a FAM-record parameter that is NOT a free-only wrapper
//     keeps the historical unrooted-free wording.
//
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/assign.c 2>&1 | FileCheck %s --check-prefix=ASSIGN
// RUN: not emitrust-import-c %t/gap-alloc.c 2>&1 | FileCheck %s --check-prefix=GAPALLOC
// RUN: not emitrust-import-c %t/gap-access.c 2>&1 | FileCheck %s --check-prefix=GAPACCESS
// RUN: not emitrust-import-c %t/typed-mismatch.c 2>&1 | FileCheck %s --check-prefix=TYPEDMIS
// RUN: not emitrust-import-c %t/typed-addform.c 2>&1 | FileCheck %s --check-prefix=TYPEDADD
// RUN: not emitrust-import-c %t/typed-gap.c 2>&1 | FileCheck %s --check-prefix=TYPEDGAP
// RUN: not emitrust-import-c %t/typed-decay-mismatch.c 2>&1 | FileCheck %s --check-prefix=TYPEDDECAY
// RUN: not emitrust-import-c %t/typed-void-view.c 2>&1 | FileCheck %s --check-prefix=TYPEDVOID
// RUN: not emitrust-import-c %t/typed-byte-view.c 2>&1 | FileCheck %s --check-prefix=TYPEDBYTE
// RUN: not emitrust-import-c %t/nonvec-tail-alloc.c 2>&1 | FileCheck %s --check-prefix=NONVECALLOC
// RUN: not emitrust-import-c %t/nonvec-tail-access.c 2>&1 | FileCheck %s --check-prefix=NONVECACCESS
// RUN: not emitrust-import-c %t/extern-ptr.c 2>&1 | FileCheck %s --check-prefix=EXTERNPTR
// RUN: not emitrust-import-c %t/rebind.c 2>&1 | FileCheck %s --check-prefix=REBIND
// RUN: not emitrust-import-c %t/free-non-wrapper.c 2>&1 | FileCheck %s --check-prefix=FREENW
// RUN: not emitrust-import-c %t/global-record.c 2>&1 | FileCheck %s --check-prefix=GLOBAL

//--- assign.c
// Whole-record assignment: C copies the fields only (never the tail); the
// owned-Vec representation has no equivalent, so the site rejects.
typedef struct {
  unsigned short n;
  unsigned char tail[];
} rec;

void copy(rec *a, rec *b) {
  *a = *b;
}
// ASSIGN: assign.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: whole-record assignment of a flexible-array-member record

//--- gap-alloc.c
// A constructible padding-gap layout: offsetof(tail)==5 < sizeof==6, so C
// may index tail[0] inside the record's own padding. The admission requires
// the gap-free layout; the allocation gets the dedicated FAM rejection.
#include <stdlib.h>
typedef struct {
  unsigned char a;
  unsigned short b;
  unsigned char c;
  unsigned char tail[];
} gap;

int use_gap(unsigned n) {
  gap *g = malloc(sizeof(gap) + n);
  g->a = 1;
  free(g);
  return 0;
}
// GAPALLOC: gap-alloc.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: unrecognized allocation of a flexible-array-member record

//--- gap-access.c
// The gap layout's tail access through a parameter keeps the historical
// located FAM wording (the record never grew a Vec field).
typedef struct {
  unsigned char a;
  unsigned short b;
  unsigned char c;
  unsigned char tail[];
} gap;

unsigned char read_gap(gap *g) {
  return g->tail[0];
}
// GAPACCESS: gap-access.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: flexible array member access

//--- typed-mismatch.c
// FR-95 frontier: the multiply factor 3 does not fold to sizeof(short)==2,
// so the element count is not extractable — the allocation keeps the
// dedicated FAM rejection (never a silently mis-sized tail).
#include <stdlib.h>
typedef struct {
  unsigned short size;
  short index[];
} hs_index;

int use_mismatch(unsigned long n) {
  hs_index *p = malloc(n * 3 + sizeof(hs_index));
  if (p == NULL) return 1;
  p->index[0] = 4;
  free(p);
  return 0;
}
// TYPEDMIS: typed-mismatch.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: unrecognized allocation of a flexible-array-member record

//--- typed-addform.c
// FR-95 frontier: FR-94's plain-add byte form over a TYPED tail — no
// multiply, so the byte count cannot be divided at compile time. The u8
// arm keeps its byte-count path; the typed arm rejects the allocation.
#include <stdlib.h>
typedef struct {
  unsigned short size;
  short index[];
} hs_index;

int use_add(unsigned long n) {
  hs_index *p = malloc(sizeof(hs_index) + n);
  if (p == NULL) return 1;
  p->index[0] = 4;
  free(p);
  return 0;
}
// TYPEDADD: typed-addform.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: unrecognized allocation of a flexible-array-member record

//--- typed-gap.c
// FR-95 frontier: the gap gate is element-independent — offsetof(t)==6 <
// sizeof==8 lets C index the tail inside the record's padding, so a typed
// gap layout keeps the alloc rejection like the u8 one.
#include <stdlib.h>
typedef struct {
  int a;
  char b;
  short t[];
} gap16;

int use_gap16(unsigned long n) {
  gap16 *p = malloc(n * sizeof(short) + sizeof(gap16));
  if (p == NULL) return 1;
  p->t[0] = 4;
  free(p);
  return 0;
}
// TYPEDGAP: typed-gap.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: unrecognized allocation of a flexible-array-member record

//--- typed-decay-mismatch.c
// FR-95 frontier: a decay of the ADMITTED typed tail to a pointer of a
// DIFFERENT element type cannot ride the cursor — even a SAME-SIZE
// mismatch (unsigned short over an i16 Vec, which a byte-granular check
// would miss) rejects: the region walk never binds the mismatched decay,
// so the assignment keeps the non-address wording (measured).
typedef struct {
  unsigned short size;
  short index[];
} hs_index;

unsigned short bad_view(hs_index *h) {
  unsigned short *w = h->index;
  return w[0];
}
// TYPEDDECAY: typed-decay-mismatch.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- typed-void-view.c
// FR-95 frontier: the void-pointee WILDCARD stays u8-only — void* cursor
// arithmetic is byte-granular and would miscompile over a Vec<i16>.
typedef struct {
  unsigned short size;
  short index[];
} hs_index;

int void_view(hs_index *h) {
  void *w = h->index;
  return w != 0;
}
// TYPEDVOID: typed-void-view.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer element type does not match its target array

//--- typed-byte-view.c
// FR-95 frontier: a byte-view CAST of the typed tail (the FR-83 byte-view
// precedent is NOT extended — the corpus has zero such sites); the cast
// value never becomes an address.
typedef struct {
  unsigned short size;
  short index[];
} hs_index;

unsigned char byte_view(hs_index *h) {
  unsigned char *b = (unsigned char *)h->index;
  return b[1];
}
// TYPEDBYTE: typed-byte-view.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- nonvec-tail-alloc.c
// FR-95 frontier: an element with no Vec mapping (long double) keeps the
// record outside the admission — its allocation gets the FAM rejection.
#include <stdlib.h>
typedef struct {
  unsigned short size;
  long double t[];
} nv;

int use_nv(unsigned long n) {
  nv *p = malloc(n * sizeof(long double) + sizeof(nv));
  if (p == NULL) return 1;
  p->t[0] = 4;
  free(p);
  return 0;
}
// NONVECALLOC: nonvec-tail-alloc.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: unrecognized allocation of a flexible-array-member record

//--- nonvec-tail-access.c
// The non-Vec-mappable element's access keeps the HISTORICAL FAM wording
// (the record never grew a Vec field) — the pre-FR-94 pin, preserved.
typedef struct {
  unsigned short size;
  long double t[];
} nv;

long double read_nv(nv *p) {
  return p->t[0];
}
// NONVECACCESS: nonvec-tail-access.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: flexible array member access

//--- extern-ptr.c
// An admitted record behind a GLOBAL pointer has no recognized allocation
// (nothing owns the tail): the shape stays located — the pointer global
// itself has no target object, so the `g->tail[0]` access never resolves.
typedef struct {
  unsigned short n;
  unsigned char tail[];
} rec;

rec *g;

unsigned char peek(void) {
  return g->tail[0];
}
// EXTERNPTR: extern-ptr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global pointer variable 'g' has no known target object

//--- rebind.c
// The re-binding form is not the declaration-bound owned-tail shape; the
// FAM-record allocation gets its dedicated rejection (never the historical
// over-counted array-of-records backing).
#include <stdlib.h>
typedef struct {
  unsigned short n;
  unsigned char tail[];
} rec;

int rebind(unsigned k) {
  rec *d;
  d = malloc(sizeof(rec) + k);
  d->n = 1;
  free(d);
  return 0;
}
// REBIND: rebind.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: unrecognized allocation of a flexible-array-member record

//--- free-non-wrapper.c
// A parameter that is WRITTEN through and then freed is not the free-only
// wrapper shape (an owned-by-value copy would take the write where C
// mutates the caller's object): the free keeps the historical unrooted
// wording. (Member READS before the free ARE the wrapper shape —
// heatshrink_decoder_free computes its byte size from the fields.)
#include <stdlib.h>
typedef struct {
  unsigned short n;
  unsigned char tail[];
} rec;

void free_after_write(rec *d) {
  d->n = 0;
  free(d);
}
// FREENW: free-non-wrapper.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: free of a pointer not rooted in a recognized allocation

//--- global-record.c
// A GLOBAL of an admitted record is the one type-position that can smuggle
// the non-Copy Vec tail behind a Copy derive (the staged-copy global model
// and the actor owner lift both require a Copy image), so it rejects
// located instead of dying at rustc (E0204/E0507).
typedef struct {
  unsigned short n;
  unsigned char tail[];
} rec;

rec g;

unsigned short read_global(void) {
  return g.n;
}
// GLOBAL: global-record.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global variable of a flexible-array-member record with an owned tail
