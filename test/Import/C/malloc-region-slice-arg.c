// RUN: split-file %s %t
// RUN: emitrust-import-c %t/shared.c | FileCheck %s --check-prefix=SHARED
// RUN: emitrust-import-c %t/mut.c | FileCheck %s --check-prefix=MUT
// RUN: emitrust-import-c %t/typed.c | FileCheck %s --check-prefix=TYPED
// RUN: emitrust-import-c %t/distinct.c | FileCheck %s --check-prefix=DISTINCT
// RUN: emitrust-import-c %t/mixed.c | FileCheck %s --check-prefix=MIXED
// RUN: emitrust-import-c %t/literal.c | FileCheck %s --check-prefix=LITERAL
// RUN: emitrust-import-c %t/cursor.c | FileCheck %s --check-prefix=CURSOR
// RUN: emitrust-import-c %t/alias-decl.c | FileCheck %s --check-prefix=ALIASDECL

// FR-147. Passing an ALLOCATION-BACKED pointer to a user-defined function
// with a slice parameter. This file pins the ADMITTED side of the frontier
// FR-146 opened; the rejected side (two arguments into ONE allocation) is
// pinned next door in malloc-region-slice-arg-invalid.c, and the two files
// have to be read together: the whole reason this shape was rejected rather
// than emitted is that `emitCall`'s aliasing guard is keyed on the
// argument's `VarDecl` ROOT, and a heap region HAS none. Admitting the
// borrow without also giving the guard a backing-keyed identity would turn
// every `g(p, p)` into a rustc E0499/E0502 crate — a build failure, not a
// diagnostic.
//
// The invariant each unit pins is that the argument is
// `emitrust.slice_of` of the ALLOCATION'S OWN BACKING ARRAY at the
// POINTER'S OWN CURSOR — the same shape a local array's decay already
// produces. A borrow of some other place (or at a hard-coded cursor 0)
// would read and write the wrong storage, which is a wrong-answer
// miscompile the companion EndToEnd byte diff exists to catch;
// `cargo build` cannot see it.
//
// The first three units are the exact inputs FR-146 pinned as
// `unsupported: passing a pointer into a heap allocation as a slice
// argument` in malloc-region-string-fn-invalid.c. The pin moves FORWARD
// here: they now import, and they stay in a test so the FR-146 SEGFAULT
// (a null `VarDecl` dereferenced while formatting that rejection) cannot
// come back through them either — a crash fails this file just as loudly
// as a wrong borrow does.

// The shared/read-only spelling: `first(p)` on a `const char *`
// parameter. `p` is one allocation, so exactly one borrow of the backing
// exists and there is nothing to collide with.
//--- shared.c
#include <stdlib.h>
static int first(const char *b) { return b[0]; }
int f(void) {
  char *p = (char *)malloc(8);
  p[0] = 7;
  return first(p);
}
// SHARED: func.func @first(%arg0: !emitrust.mut_ref<!emitrust.slice<i8>>)
// SHARED-LABEL: func.func @f()
// SHARED: %[[CELL:.*]] = memref.alloca() : memref<i64>
// SHARED: %[[BACK:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>
// SHARED: emitrust.assign
// SHARED-NEXT: %[[CUR:.*]] = memref.load %[[CELL]][] : memref<i64>
// SHARED-NEXT: %[[REF:.*]] = emitrust.slice_of mut %[[BACK]][%[[CUR]]] : (!emitrust.lvalue<!emitrust.array<8xi8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>
// SHARED-NEXT: call @first(%[[REF]])

// The MUTABLE-parameter spelling reaches the same guard, and the callee's
// write has to land in the caller's backing (the EndToEnd diff checks the
// value; this checks it is the backing that is borrowed, not a copy).
//--- mut.c
#include <stdlib.h>
static void fill(char *b) { b[0] = 3; }
int f(void) {
  char *p = (char *)malloc(8);
  fill(p);
  return p[0];
}
// MUT-LABEL: func.func @f()
// MUT: %[[CELL:.*]] = memref.alloca() : memref<i64>
// MUT: %[[BACK:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>
// MUT: memref.store %{{.*}}, %[[CELL]][] : memref<i64>
// MUT-NEXT: %[[CUR:.*]] = memref.load %[[CELL]][] : memref<i64>
// MUT-NEXT: %[[REF:.*]] = emitrust.slice_of mut %[[BACK]][%[[CUR]]]
// MUT-NEXT: call @fill(%[[REF]])

// A TYPED (non-byte) allocation is admitted on the same terms: the
// backing's element type must AGREE with the slice parameter's, so the
// borrow can never reinterpret 8 bytes as a differently sized element.
//--- typed.c
#include <stdlib.h>
static int head(int *b) { return b[0]; }
int f(void) {
  int *p = (int *)malloc(8);
  p[0] = 7;
  return head(p);
}
// TYPED: func.func @head(%arg0: !emitrust.mut_ref<!emitrust.slice<i32>>)
// TYPED-LABEL: func.func @f()
// TYPED: %[[BACK:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<2xi32>>
// TYPED: emitrust.slice_of mut %[[BACK]][{{.*}}] : (!emitrust.lvalue<!emitrust.array<2xi32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>

// Two DISTINCT allocations in one call. This is the case the backing key
// must ADMIT: `p` and `r` have different alloc sites, hence different
// backings, hence disjoint storage in C and two legal simultaneous
// borrows in Rust. A key that collapsed all heap regions into one bucket
// would reject this and lose the idiom entirely.
//--- distinct.c
#include <stdlib.h>
static int g(const char *a, const char *b) { return a[0] + b[0]; }
int f(void) {
  char *p = (char *)malloc(8);
  char *r = (char *)malloc(8);
  p[0] = 3;
  r[0] = 4;
  return g(p, r);
}
// DISTINCT-LABEL: func.func @f()
// DISTINCT: %[[P:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>
// DISTINCT: %[[R:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>
// DISTINCT: emitrust.slice_of mut %[[P]]
// DISTINCT: emitrust.slice_of mut %[[R]]
// DISTINCT: call @g(

// A NAMED root and a heap region in the same call. The two key spaces are
// disjoint by construction (a synthesized backing is nobody's declared
// object), so neither guard can see the other's borrow and the pair is
// admitted — as it must be, since a local array and a fresh allocation
// cannot overlap.
//--- mixed.c
#include <stdlib.h>
static int g(const char *a, const char *b) { return a[0] + b[0]; }
int f(void) {
  char q[8];
  char *p = (char *)malloc(8);
  q[0] = 1;
  p[0] = 2;
  return g(q, p);
}
// MIXED-LABEL: func.func @f()
// MIXED: %[[Q:.*]] = emitrust.variable named "q" : !emitrust.lvalue<!emitrust.array<8xi8>>
// MIXED: %[[P:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>
// MIXED: emitrust.slice_of mut %[[Q]]
// MIXED: emitrust.slice_of mut %[[P]]
// MIXED: call @g(

// A heap region and a STRING-LITERAL backing in the same call. Both are
// root-less regions (`root` is null for each), which is exactly the
// situation the old single-key guard could not tell apart. They are
// different backing places, so the pair is admitted; a key that lumped all
// root-less borrows together would reject this, and one that ignored them
// would emit two borrows of one place.
//--- literal.c
#include <stdlib.h>
static int g(const char *a, const char *b) { return a[0] + b[0]; }
int f(void) {
  char *p = (char *)malloc(8);
  p[0] = 1;
  return g(p, "abc");
}
// LITERAL-LABEL: func.func @f()
// LITERAL: %[[BACK:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>
// LITERAL: emitrust.slice_of mut %[[BACK]][{{.*}}] : (!emitrust.lvalue<!emitrust.array<8xi8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>
// LITERAL-NEXT: %[[LIT:.*]] = emitrust.variable <[97 : i8, 98 : i8, 99 : i8, 0 : i8]> : !emitrust.lvalue<!emitrust.array<4xi8>>
// LITERAL: emitrust.slice_of mut %[[LIT]]
// LITERAL-NEXT: call @g(

// A NONZERO, RUNTIME cursor. The borrow starts at the pointer's own
// cursor cell, not at 0 — the window-placement property the byte diff
// re-checks with a runtime seed.
//--- cursor.c
#include <stdlib.h>
static int g(const char *b) { return b[0]; }
int f(int n) {
  char *p = (char *)malloc(8);
  p[0] = 3;
  return g(p + (n & 1));
}
// CURSOR-LABEL: func.func @f(
// CURSOR: %[[CELL:.*]] = memref.alloca() : memref<i64>
// CURSOR: %[[BACK:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>
// CURSOR: emitrust.assign
// CURSOR-NEXT: %[[BASE:.*]] = memref.load %[[CELL]][] : memref<i64>
// CURSOR: %[[OFF:.*]] = arith.addi %[[BASE]], %{{.*}} : i64
// CURSOR-NEXT: %[[REF:.*]] = emitrust.slice_of mut %[[BACK]][%[[OFF]]]
// CURSOR-NEXT: call @g(%[[REF]])

// FR-147 also fixes a DEFECT found while admitting the shape above: a
// SECOND pointer local united into an allocation region declared with an
// INITIALIZER (`char *q = p + 3;`) had that initializer silently dropped
// — the declaration only stored cursor 0 — so `q` addressed the
// allocation from offset 0. The assignment spelling `q = p + 3` was
// already correct, so this pins that the declaration now stores the same
// cursor the assignment does.
//--- alias-decl.c
#include <stdlib.h>
int f(void) {
  char *p = (char *)malloc(8);
  p[3] = 9;
  char *q = p + 3;
  return q[0];
}
// ALIASDECL-LABEL: func.func @f()
// ALIASDECL: %[[QCELL:.*]] = memref.alloca() : memref<i64>
// ALIASDECL: %[[PCELL:.*]] = memref.alloca() : memref<i64>
// ALIASDECL: emitrust.assign
// ALIASDECL: memref.store %{{.*}}, %[[QCELL]][] : memref<i64>
// ALIASDECL-NEXT: %[[PCUR:.*]] = memref.load %[[PCELL]][] : memref<i64>
// ALIASDECL: %[[QCUR:.*]] = arith.addi %[[PCUR]], %{{.*}} : i64
// ALIASDECL-NEXT: memref.store %[[QCUR]], %[[QCELL]][] : memref<i64>
