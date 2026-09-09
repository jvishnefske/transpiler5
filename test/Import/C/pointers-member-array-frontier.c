// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/ptr-to-ptr.c 2>&1 | FileCheck %s --check-prefix=PTRPTR
// RUN: not emitrust-import-c %t/two-dim.c 2>&1 | FileCheck %s --check-prefix=TWOD
// RUN: emitrust-import-c %t/union-arm.c 2>&1 | FileCheck %s --check-prefix=UNIONARM
// RUN: not emitrust-import-c %t/volatile-elem.c 2>&1 | FileCheck %s --check-prefix=VOLATILE
// RUN: not emitrust-import-c %t/file-ptr.c 2>&1 | FileCheck %s --check-prefix=FILEPTR
// RUN: not emitrust-import-c %t/local-var.c 2>&1 | FileCheck %s --check-prefix=LOCALVAR
// RUN: not emitrust-import-c %t/global-var.c 2>&1 | FileCheck %s --check-prefix=GLOBALVAR
// RUN: emitrust-import-c %t/fnptr-table.c | FileCheck %s --check-prefix=FNPTR
// RUN: not emitrust-import-c %t/fnptr-table-store.c 2>&1 | FileCheck %s --check-prefix=FNPTRSTORE
// RUN: emitrust-import-c %t/fam-tail.c | FileCheck %s --check-prefix=FAMTAIL
// RUN: emitrust-import-c %t/probe.cpp | FileCheck %s --check-prefix=CPP

// FR-107 frontier: the `[i64; N]` admission covers EXACTLY a
// CONSTANT-length array whose element is a plain DATA pointer to a
// non-pointer. Everything outside that keeps its pre-existing LOCATED
// rejection, and the two shapes that ALREADY worked keep working
// byte-for-byte — a widening that happened by accident would be as much
// a behavior change as a narrowing.
//
//  - an array of POINTER-TO-POINTER (`int **m[4]`): the slot model has no
//    representation for a cursor whose pointee is itself a cursor, and the
//    `getPointeeType()->isPointerType()` guard is the ONLY thing keeping
//    it out. Dropping that guard silently widens the frontier.
//  - a MULTI-DIMENSIONAL pointer array (`int *m[2][3]`): the single
//    `getAsConstantArrayType` peel sees an element of ARRAY type, not a
//    data pointer, so this arm is what pins that the peel stays one deep.
//  - a UNION arm: union arms map through `mapType`, not
//    `mapStructFieldType`, so the admission deliberately does not reach
//    them; a one-slot aliasing model over cursor slots has no meaning.
//    FR-167 PHASE 2 moved this leg's PIN, not this frontier: the arm
//    still gets no `[i64; N]`, but the containing union now falls back to
//    FR-78's opaque blob instead of rejecting, so the leg is a positive
//    check with a `-NOT` guarding that the pointer wording stays gone.
//  - a VOLATILE-qualified element: the volatile scan runs FIRST in
//    `mapStructFieldType`, before any pointer shortcut, and must keep
//    running first.
//  - a FILE* element: an owned stream handle is function-local only, and
//    an array of them must not become inert slots.
//  - a pointer array as a LOCAL or a GLOBAL variable: a separate front
//    entirely (nothing about FR-107 touches non-member positions).
//  - the fn-pointer TABLE (`int (*tab[3])(int)`) already maps to a REAL
//    `!emitrust.fn_ptr` array and already rejects slot stores. A function
//    pointer is not a data pointer, so FR-107 must not touch it.
//  - the FAM tail of pointers (`int *m[]`) is already tolerated as a
//    dropped field under FR-94/95 — an incomplete array is not a
//    `ConstantArrayType`, so FR-107 must not accidentally give it a slot
//    run.
//  - C++ is INCLUDED by deliberate choice: `mapStructFieldType` is shared
//    between the two front ends, the representation is language-neutral,
//    and every element use rejects through the same shared paths. Pinned
//    here so the choice is a decision and not an accident.

//--- ptr-to-ptr.c
struct H { int **m[4]; };
struct H g;
// PTRPTR: ptr-to-ptr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer type outside a parameter position

//--- two-dim.c
struct H { int *m[2][3]; };
struct H g;
// TWOD: two-dim.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer type outside a parameter position

//--- union-arm.c
// FR-167 PHASE 2 MOVED THIS PIN FORWARD. A pointer-array arm still has no
// `[i64; N]` admission -- the FR-107 widening deliberately stops at a
// STRUCT MEMBER -- but the union that contains it is no longer killed by
// that: `collectUnionSlot`'s refusal is now a TRIAL, and the fallback is
// FR-78's sizeof-sized opaque blob (16 bytes here). The pointer arm gets
// no representation and every access through it is a located rejection;
// only the DECLARATION moved. `-NOT` guards that the pointer rejection
// does not ALSO surface -- a union that imports must not print an error.
union U { int *m[2]; int n; };
union U g;
// UNIONARM: emitrust.struct_def @U ["opaque"] [!emitrust.array<16xui8>] {{.*}}emitrust.opaque_union}
// UNIONARM-NOT: pointer type outside a parameter position

//--- volatile-elem.c
struct H { int *volatile m[4]; };
struct H g;
// VOLATILE: volatile-elem.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: volatile-qualified type

//--- file-ptr.c
#include <stdio.h>
struct H { FILE *m[4]; };
struct H g;
// FILEPTR: file-ptr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: FILE* is only supported as a function-local variable

//--- local-var.c
int main(void) { int *m[4]; return 0; }
// LOCALVAR: local-var.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer type outside a parameter position

//--- global-var.c
int *gm[4];
// GLOBALVAR: global-var.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer type outside a parameter position

//--- fnptr-table.c
struct T { int (*tab[3])(int); };
struct T g;
// FNPTR: emitrust.struct_def @T ["tab"] [!emitrust.array<3x!emitrust.fn_ptr<(i32) -> i32>>]

//--- fnptr-table-store.c
struct T { int (*tab[3])(int); };
struct T g;
static int one(int v) { return v + 1; }
int main(void) { g.tab[0] = one; return 0; }
// FNPTRSTORE: fnptr-table-store.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: assignment to a function-pointer array element

//--- fam-tail.c
struct S { int n; int *m[]; };
int f(struct S *s) { return s->n; }
// FAMTAIL: emitrust.struct_def @S ["n"] [i32]

//--- probe.cpp
struct T { int tag; };
struct H { int n; T *m[4]; };
H g;
int main() { return g.n; }
// CPP: emitrust.struct_def @H ["n", "m"] [i32, !emitrust.array<4xi64>]
