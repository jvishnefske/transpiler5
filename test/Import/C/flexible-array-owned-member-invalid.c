// FR-96 frontier: what the member-held owned-FAM-record lift does NOT claim,
// pinned so nothing silently emits wrong code.
//   - Address-of the member (`&e->search_index`) escapes the Option-member
//     model: the second-order binding keeps its historical rejection (and
//     the field poisons, so it never lifts).
//   - Pointer arithmetic on the member (`e->search_index++`) needs runtime
//     cursor state the owned member does not have.
//   - The member-read local may not ESCAPE the per-use projection: stored to
//     a global it keeps the non-address rejection; returned it keeps the
//     returned-pointer rejection (the FR-94 owned-return arm only claims
//     famAllocLocals, never member-read locals).
//   - A whole-record assignment of the CONTAINER keeps FR-94's rejection —
//     that arm is exactly what makes the Option field sound (no fields-only
//     copy can duplicate or drop the owned payload).
//   - An UNRECOGNIZED alloc form into the member (no sizeof(S) addend)
//     poisons the field: every use keeps the member-wall wording — never a
//     silently mis-sized Some.
//   - A SELF-REFERENTIAL pointer field (pointee record == container) is
//     excluded (an `Option<S>` field inside S would be an infinite-size
//     type): it stays on the member wall.
//   - SCOPE (this wave): only containers that are THEMSELVES admitted FAM
//     records (already non-Copy) lift the member. A NON-FAM container keeps
//     its historical i64 slot AND its whole-record Copy semantics — pinned
//     POSITIVE below so the scope boundary cannot drift silently.
//
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/addrof.c 2>&1 | FileCheck %s --check-prefix=ADDROF
// RUN: not emitrust-import-c %t/arith.c 2>&1 | FileCheck %s --check-prefix=ARITH
// RUN: not emitrust-import-c %t/escape-global.c 2>&1 | FileCheck %s --check-prefix=ESCGLOB
// RUN: not emitrust-import-c %t/escape-return.c 2>&1 | FileCheck %s --check-prefix=ESCRET
// RUN: not emitrust-import-c %t/container-copy.c 2>&1 | FileCheck %s --check-prefix=CONTCOPY
// RUN: not emitrust-import-c %t/bad-alloc.c 2>&1 | FileCheck %s --check-prefix=BADALLOC
// RUN: not emitrust-import-c %t/self-ref.c 2>&1 | FileCheck %s --check-prefix=SELFREF
// RUN: emitrust-import-c %t/non-fam-container.c | FileCheck %s --check-prefix=NONFAM

//--- addrof.c
// `&e->search_index` escapes the static model: the pointer-to-pointer local
// keeps its historical rejection wording (measured).
#include <stdlib.h>
struct hs_index { unsigned short size; short index[]; };
struct enc { unsigned short n; struct hs_index *search_index; unsigned char buffer[]; };

void grab(struct enc *e) {
  struct hs_index **p = &e->search_index;
  *p = NULL;
}
// ADDROF: addrof.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-to-pointer variable assigned a value that is not the address of a local pointer variable

//--- arith.c
// Walking the member pointer needs runtime cursor state (measured wording).
struct hs_index { unsigned short size; short index[]; };
struct enc { unsigned short n; struct hs_index *search_index; unsigned char buffer[]; };

void bump(struct enc *e) {
  e->search_index++;
}
// ARITH: arith.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: ++/-- on this pointer expression

//--- escape-global.c
// The member-read local escaping to a global would outlive every projection;
// the global's region keeps the non-address rejection (measured).
struct hs_index { unsigned short size; short index[]; };
struct enc { unsigned short n; struct hs_index *search_index; unsigned char buffer[]; };

struct hs_index *g;

void peek(struct enc *e) {
  struct hs_index *hsi = e->search_index;
  g = hsi;
}
// ESCGLOB: escape-global.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- escape-return.c
// Returning the member-read local: member-read locals are never
// famAllocLocals, so the owned-return arm cannot claim the function and the
// returned-pointer rejection stays (measured, full wording).
struct hs_index { unsigned short size; short index[]; };
struct enc { unsigned short n; struct hs_index *search_index; unsigned char buffer[]; };

struct hs_index *peek(struct enc *e) {
  struct hs_index *hsi = e->search_index;
  return hsi;
}
// ESCRET: escape-return.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: returned pointer value (only a returned function address has a representation; a cursor into a callee-local region would dangle)

//--- container-copy.c
// The FR-94 whole-record rejection covers the member-holding container: a
// fields-only C copy could duplicate the owned payload, so the site rejects.
struct hs_index { unsigned short size; short index[]; };
struct enc { unsigned short n; struct hs_index *search_index; unsigned char buffer[]; };

void clone_enc(struct enc *a, struct enc *b) {
  *a = *b;
}
// CONTCOPY: container-copy.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: whole-record assignment of a flexible-array-member record

//--- bad-alloc.c
// An alloc form the planner cannot size (no sizeof(struct hs_index) addend)
// poisons the field: the write keeps the member-wall wording and note.
#include <stdlib.h>
struct hs_index { unsigned short size; short index[]; };
struct enc { unsigned short n; struct hs_index *search_index; unsigned char buffer[]; };

void bad_alloc(struct enc *e, unsigned long n) {
  e->search_index = malloc(n);
}
// BADALLOC: bad-alloc.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member 'search_index' is used outside the static-binding model
// BADALLOC: note: first unresolvable use is here

//--- self-ref.c
// A self-referential member (pointee == container) would be an
// infinite-size Option field; it stays on the member wall.
#include <stdlib.h>
struct node { unsigned short n; struct node *next; unsigned char tail[]; };

void link_node(struct node *a) {
  a->next = malloc(sizeof(struct node) + 4);
}
// SELFREF: self-ref.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member 'next' is used outside the static-binding model

//--- non-fam-container.c
// SCOPE pin (positive): a NON-FAM container holding the same member imports
// TODAY with the member as a plain i64 static-binding slot and whole-struct
// copies allowed; lifting it needs the translate Copy-drop generalization
// (an `Option<` arm) plus the assignment rejection — deferred, so the shape
// must keep importing byte-identically.
struct hs_index { unsigned short size; short index[]; };
struct enc2 { unsigned short n; struct hs_index *search_index; };

void clone2(struct enc2 *a, struct enc2 *b) {
  *a = *b;
}
// NONFAM: emitrust.struct_def @enc2 ["n", "search_index"] [ui16, i64]
// NONFAM-LABEL: func.func @clone2
// NONFAM: %[[V:.*]] = emitrust.load %{{.*}} : (!emitrust.lvalue<!emitrust.struct<"enc2">>) -> !emitrust.struct<"enc2">
// NONFAM-NEXT: emitrust.assign %{{.*}} = %[[V]] : !emitrust.lvalue<!emitrust.struct<"enc2">>
