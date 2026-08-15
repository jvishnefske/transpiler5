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
//   - A non-u8 FAM leaf (heatshrink's second FAM record, hs_index's
//     `int16_t index[]`) keeps the historical FAM access wording.
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
// RUN: not emitrust-import-c %t/non-u8-tail.c 2>&1 | FileCheck %s --check-prefix=NONU8
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

//--- non-u8-tail.c
// heatshrink's hs_index shape: an int16_t FAM leaf is outside the byte-tail
// admission and keeps the historical wording.
typedef struct {
  unsigned short size;
  short index[];
} hs_index;

short read_index(hs_index *idx) {
  return idx->index[0];
}
// NONU8: non-u8-tail.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: flexible array member access

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
