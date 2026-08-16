// FR-98 frontier: what the FAM-tail-local-through-Option-member composition
// does NOT claim — and the exact boundary of what composes — pinned so
// nothing silently emits wrong code.
//   - The tail local passed to a SLICE-classified callee (the callee walks
//     its parameter): the member-read root has no `symbols` place to
//     reslice a borrow from, so the argument keeps a located rejection.
//   - Rebinding across two member-read roots joins two disjoint Option
//     payloads into one region: the historical join rejection, naming both
//     objects and both binding sites (the multi-base member model
//     element-checks the FAM member type and can never admit an
//     incomplete-array member, so no dispatch form exists).
//   - A member-read root that is itself REASSIGNED is never recognized
//     (planFamLift's mutatesVar gate), so the tail decay keeps the
//     historical non-address rejection at the binding.
//   - ROOT MOTION (the miscompile gate): a MOVED slice-param root (`hse++`
//     between binding and use) must never import — the member base is
//     re-subscripted at the root's CURRENT cursor per use, so a moved root
//     would silently retarget the local (fr98-f5, the regression pin).
//     The region model records no arithmetic fact for parameters, so the
//     gate is the AST-level mutatesVar re-check at the use.
// MEASURED COMPOSE BOUNDARY (positive arms, the owned-root FR-94/95
// precedent one projection deeper — pinned so the boundary cannot drift):
//   - The tail local passed to an UNDEFINED scalar-ref callee borrows the
//     projected element at the current cursor (`&mut ...index[cur]`).
//   - `&index` + `**pp = 3` decomposes the second-order pointer into the
//     direct projected write.
//   - A NON-CONST tail local rebound MID-LOOP to the SAME member resets
//     its cursor to 0 — a legal rebinding, not a join.
//
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/escape-slice.c 2>&1 | FileCheck %s --check-prefix=ESCSLICE
// RUN: not emitrust-import-c %t/cross-root.c 2>&1 | FileCheck %s --check-prefix=CROSSROOT
// RUN: not emitrust-import-c %t/rebound-root.c 2>&1 | FileCheck %s --check-prefix=REBIND
// RUN: not emitrust-import-c %t/root-walk.c 2>&1 | FileCheck %s --check-prefix=ROOTWALK
// RUN: emitrust-import-c %t/escape-scalar.c | FileCheck %s --check-prefix=ESCSCALAR
// RUN: emitrust-import-c %t/addrof.c | FileCheck %s --check-prefix=ADDROF
// RUN: emitrust-import-c %t/rebind-same.c | FileCheck %s --check-prefix=REBINDSAME

//--- escape-slice.c
// The callee walks its parameter, so the argument must reslice a region
// base place — the member-read root binds none (measured wording).
#include <stdint.h>
struct hs_index { uint16_t size; int16_t index[]; };
struct enc { uint16_t n; struct hs_index *search_index; uint8_t buffer[]; };
static void fill(int16_t *p, uint16_t n) {
  for (uint16_t i = 0; i < n; i++) p[i] = (int16_t)i;
}
void f(struct enc *hse) {
  struct hs_index *hsi = hse->search_index;
  int16_t *const index = hsi->index;
  fill(index, hse->n);
}
// ESCSLICE: escape-slice.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer target 'hsi' is not an importable place

//--- cross-root.c
// Rebinding across two member-read roots: the historical join rejection.
#include <stdint.h>
struct hs_index { uint16_t size; int16_t index[]; };
struct enc { uint16_t n; struct hs_index *si1; struct hs_index *si2; uint8_t buffer[]; };
void f(struct enc *hse) {
  struct hs_index *a = hse->si1;
  struct hs_index *b = hse->si2;
  int16_t *index = a->index;
  index[0] = 1;
  index = b->index;
  index[0] = 2;
}
// CROSSROOT: cross-root.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer 'index' would join objects 'a' and 'b' into one region
// CROSSROOT: note: bound to 'a' here
// CROSSROOT: note: bound to 'b' here

//--- rebound-root.c
// The member-read root reassigned before the binding: planFamLift's
// mutatesVar gate never records it, so the tail decay keeps the historical
// non-address rejection at the binding.
#include <stdint.h>
struct hs_index { uint16_t size; int16_t index[]; };
struct enc { uint16_t n; struct hs_index *si1; struct hs_index *si2; uint8_t buffer[]; };
void f(struct enc *hse) {
  struct hs_index *hsi = hse->si1;
  hsi = hse->si2;
  int16_t *const index = hsi->index;
  index[0] = 1;
}
// REBIND: rebound-root.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- root-walk.c
// THE MISCOMPILE GATE: the slice-param root walks between the tail-local
// binding and its use; following the moved cursor would silently retarget
// `data`, so the shape must NEVER import (regression pin, fr98-f5).
#include <stdint.h>
struct enc { uint16_t n; uint8_t buffer[]; };
static uint16_t get_off(struct enc *hse) { (void)hse; return 4; }
void f(struct enc *hse) {
  uint8_t *const data = hse->buffer;
  hse++;
  const uint16_t off = get_off(hse);
  data[0] = (uint8_t)off;
}
// ROOTWALK: root-walk.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: member-array binding rooted at a struct-pointer parameter that is itself modified

//--- escape-scalar.c
// COMPOSE pin: an undefined scalar-ref callee receives a fresh borrow of
// the PROJECTED element at the local's current cursor — the owned-root
// FR-94 escape behavior, one projection deeper.
#include <stdint.h>
struct hs_index { uint16_t size; int16_t index[]; };
struct enc { uint16_t n; struct hs_index *search_index; uint8_t buffer[]; };
void take(int16_t *p);
void f(struct enc *hse) {
  struct hs_index *hsi = hse->search_index;
  int16_t *const index = hsi->index;
  take(index);
}
// ESCSCALAR-LABEL: func.func @f
// ESCSCALAR: %[[OPT:.*]] = emitrust.member %{{.*}}["search_index"] : (!emitrust.lvalue<!emitrust.struct<"enc">>) -> !emitrust.lvalue<!emitrust.opaque<"Option<hs_index>">>
// ESCSCALAR-NEXT: %[[BOR:.*]] = emitrust.method_call %[[OPT]]["as_mut().unwrap"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<hs_index>">>) -> !emitrust.mut_ref<!emitrust.struct<"hs_index">>
// ESCSCALAR-NEXT: %[[HSI:.*]] = emitrust.deref %[[BOR]]
// ESCSCALAR-NEXT: %[[IDX:.*]] = emitrust.member %[[HSI]]["index"]
// ESCSCALAR-NEXT: %[[ELT:.*]] = emitrust.subscript %[[IDX]][%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>, i64) -> !emitrust.lvalue<i16>
// ESCSCALAR-NEXT: %[[REF:.*]] = emitrust.addr_of mut %[[ELT]] : (!emitrust.lvalue<i16>) -> !emitrust.mut_ref<i16>
// ESCSCALAR-NEXT: call @take(%[[REF]]) : (!emitrust.mut_ref<i16>) -> ()

//--- addrof.c
// COMPOSE pin: the second-order pointer decomposes; the write through it
// lands on the fresh projected element place (the fr98-f2b owned-root
// precedent).
#include <stdint.h>
struct hs_index { uint16_t size; int16_t index[]; };
struct enc { uint16_t n; struct hs_index *search_index; uint8_t buffer[]; };
void f(struct enc *hse) {
  struct hs_index *hsi = hse->search_index;
  int16_t *index = hsi->index;
  int16_t **pp = &index;
  **pp = 3;
}
// ADDROF-LABEL: func.func @f
// ADDROF: %[[AOPT:.*]] = emitrust.member %{{.*}}["search_index"] : (!emitrust.lvalue<!emitrust.struct<"enc">>) -> !emitrust.lvalue<!emitrust.opaque<"Option<hs_index>">>
// ADDROF: %[[AELT:.*]] = emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>, i64) -> !emitrust.lvalue<i16>
// ADDROF: emitrust.assign %[[AELT]] = %{{.*}} : !emitrust.lvalue<i16>

//--- rebind-same.c
// COMPOSE pin: a non-const tail local rebound mid-loop to the SAME member
// stays one region (one base) — the rebinding just resets the cursor.
#include <stdint.h>
struct hs_index { uint16_t size; int16_t index[]; };
struct enc { uint16_t n; struct hs_index *search_index; uint8_t buffer[]; };
void f(struct enc *hse) {
  struct hs_index *hsi = hse->search_index;
  int16_t *index = hsi->index;
  for (uint16_t i = 0; i < 4; i++) {
    index[i] = 1;
    if (i == 2) index = hsi->index;
  }
}
// REBINDSAME-LABEL: func.func @f
// REBINDSAME: emitrust.method_call %{{.*}}["as_mut().unwrap"] ()
// REBINDSAME: emitrust.assign %{{.*}} = %{{.*}} : !emitrust.lvalue<i16>
