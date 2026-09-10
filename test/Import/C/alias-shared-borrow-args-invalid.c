// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/same-ptr.c 2>&1 | FileCheck %s --check-prefix=SAMEPTR
// RUN: not emitrust-import-c %t/cursor.c 2>&1 | FileCheck %s --check-prefix=CURSOR
// RUN: not emitrust-import-c %t/obj-field.c 2>&1 | FileCheck %s --check-prefix=OBJFIELD
// RUN: not emitrust-import-c %t/mut-then-shared.c 2>&1 | FileCheck %s --check-prefix=MUTSHARED
// RUN: not emitrust-import-c %t/shared-then-mut.c 2>&1 | FileCheck %s --check-prefix=SHAREDMUT
// RUN: not emitrust-import-c %t/heap-mut.c 2>&1 | FileCheck %s --check-prefix=HEAPMUT

// FR-222's LOAD-BEARING guard. FR-222 makes `emitCall`'s aliasing
// collision key MUTABILITY-AWARE so that two SHARED borrows of one object
// stop being refused (that admitted side is pinned in
// alias-shared-borrow-args.c and byte-diffed in
// test/EndToEnd/alias-shared-borrow-args.c). This file pins what stops
// that from becoming an E0499/E0502 generator: the rule is
// `(argIsMut || heldIsMut)`, NOT "never collide", so a mutable borrow on
// EITHER side of the pair still rejects LOCATED. Every shape below would
// otherwise emit a crate that only rustc rejects — a build failure rather
// than a diagnostic, which `emitrust-import-c` would report as success.
//
// The rule is copied verbatim from `emitCXXMemberCall` (FR-203), whose own
// comment already claimed `emitCall` applied it; before FR-222 it did not,
// and the two copies disagreed. One rule, one message: every wording here
// is FR-48's existing sentence, unchanged, and is pinned as measured.
//
// Read together with the neighbouring frontier files, which pin the shapes
// FR-222 deliberately does NOT touch: two sibling fields of one struct
// (member-array-arg-invalid.c / FR-74), the heap two-cursor's wording and
// its null-root safety (malloc-region-slice-arg-invalid.c / FR-146), and
// the caller-LOCAL array two-cursor that the Phase-4 owner lift already
// admits by turning both pointers into i64 indices (FR-201).

// The SAME pointer twice into two MUTABLE slice parameters. Two `&mut`
// of one region: rustc E0499 if emitted.
//--- same-ptr.c
typedef unsigned char u8;
static int f(u8 *a, u8 *b) { a[0] = 1; return a[0] + b[0]; }
int run(u8 *p) { return f(p, p); }
// SAMEPTR: same-ptr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aliasing mutable pointer arguments (two arguments borrow object 'p')

// A pointer and a cursor DERIVED from it, both mutable. The windows are
// open-ended, so they overlap however far the cursor is advanced.
//--- cursor.c
typedef unsigned char u8;
static int f(u8 *a, u8 *b) { a[0] = 1; return a[0] + b[0]; }
int run(u8 *p) { return f(p, p + 1); }
// CURSOR: cursor.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aliasing mutable pointer arguments (two arguments borrow object 'p')

// An object and its OWN field: the empty member path prefix-overlaps the
// one-element path, and the whole-object borrow is mutable.
//--- obj-field.c
typedef unsigned char u8;
typedef struct { u8 x[4]; u8 y[4]; } T;
static int f(T *t, u8 *b) { t->x[0] = 1; return t->x[0] + b[0]; }
int run(T *t) { return f(t, t->y); }
// OBJFIELD: obj-field.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aliasing mutable pointer arguments (two arguments borrow object 't')

// MUT first, SHARED second. `&mut p[8..]` beside `&p[..]` is rustc E0502;
// exactly one mutable side is enough to collide.
//--- mut-then-shared.c
typedef unsigned char u8;
static void f(u8 *out, const u8 *in) { out[0] = in[1]; }
void run(u8 *p) { f(p + 8, p); }
// MUTSHARED: mut-then-shared.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aliasing mutable pointer arguments (two arguments borrow object 'p')

// SHARED first, MUT second — the same pair in the other argument order.
// The rule is symmetric because the HELD borrow's mutability is consulted
// as well as the incoming one; a rule that only looked at the incoming
// argument would let this one through.
//--- shared-then-mut.c
typedef unsigned char u8;
static void f(const u8 *in, u8 *out) { out[0] = in[1]; }
void run(u8 *p) { f(p, p + 8); }
// SHAREDMUT: shared-then-mut.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aliasing mutable pointer arguments (two arguments borrow object 'p')

// The heap two-cursor with a MUTABLE side. A heap region has no `VarDecl`
// root, so it rides the backing-keyed half of the key (FR-147); FR-222
// made that half mutability-aware too, and this pins that it still names
// the ALLOCATION and never a decl — formatting `root->getName()` on a
// heap borrow's null root is the FR-146 segfault.
//--- heap-mut.c
#include <stdlib.h>
typedef unsigned char u8;
static int f(u8 *a, u8 *b) { a[0] = 1; return a[0] + b[0]; }
int run(void) {
  u8 *h = (u8 *)malloc(64);
  h[0] = 3;
  int r = f(h, h + 4);
  free(h);
  return r;
}
// HEAPMUT: heap-mut.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aliasing mutable pointer arguments (two arguments borrow the same heap allocation)
// HEAPMUT-NOT: error: unsupported: aliasing mutable pointer arguments (two arguments borrow object
