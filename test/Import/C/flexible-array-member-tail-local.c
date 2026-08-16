// RUN: emitrust-import-c %s | FileCheck %s

// FR-98: FAM-tail pointer LOCALS through Option-member projections — the
// do_indexing front (heatshrink_encoder.c:425): `int16_t *const index =
// hsi->index;` where `hsi` is itself the FR-96 member-read local over the
// lifted `Option<hs_index>` field, PLUS `uint8_t *const data = hse->buffer;`
// where `hse` is a SLICE-classified struct-pointer parameter (it escapes to
// a call). Two composition arms, both required to port do_indexing:
//   Arm A (member-read-local root): the local's backing is the projected
//     member's tail Vec — each USE re-projects
//     `hse[cur].search_index.as_mut().unwrap().index[cursor + i]` FRESH
//     (the FR-96 per-use discipline extended through one more member link
//     plus the tail subscript). The root local binds NO code; the tail
//     local carries only the FR-93 i64 cursor cell (init 0, const binding,
//     pure subscript — do_indexing never walks it).
//   Arm B (slice-region param root): `hse`'s place is its deref'd struct
//     slice, so the tail local's member base first subscripts the slice at
//     the ROOT'S OWN cursor — the identical load the direct `hse->member`
//     path emits — then projects the tail Vec. Sound only for an UNWALKED
//     root; the moved-root gate lives in the invalid file (ROOTWALK).
// The loop interleaves an indexed WRITE through the Arm-A local, a window
// READ through the Arm-B local, and a direct member use of the same root —
// two disjoint field paths of one root, the FR-90/93 disjointness precedent
// one projection deeper; per-use projections keep it NLL-safe (byte-diff
// proven in the FR-98 spike, fr98-hand.crate vs clang native).
// The frontier (escape, address-of over these roots, cross-root joins,
// rebound member-read root, walked root) stays in
// flexible-array-member-tail-local-invalid.c.

#include <stdint.h>
#include <stdlib.h>

struct hs_index {
  uint16_t size;
  int16_t index[];
};

struct enc {
  uint16_t input_size;
  struct hs_index *search_index;
  uint8_t buffer[];
};

static uint16_t get_input_offset(struct enc *hse) {
  (void)hse;
  return 8;
}

void do_indexing(struct enc *hse) {
  struct hs_index *hsi = hse->search_index; /* FR-96 member-read local */
  uint8_t *const data = hse->buffer;        /* Arm B: slice-param root  */
  int16_t *const index = hsi->index;        /* Arm A: member-read root  */
  const uint16_t input_offset = get_input_offset(hse); /* slice-classifies hse */
  const uint16_t end = input_offset + hse->input_size; /* direct member use */
  for (uint16_t i = 0; i < end; i++) {
    uint8_t v = data[i];       /* window read through the Arm-B local  */
    index[i] = (int16_t)v;     /* tail WRITE through the Arm-A local   */
  }
}

// The tail locals bind only their i64 cursor cells (stored 0); the
// member-read local binds NOTHING (FR-96), and no whole-borrow of the
// Option payload is ever let-bound.
// CHECK-LABEL: func.func @do_indexing
// CHECK-NOT: emitrust.variable named "hsi"
// CHECK-NOT: emitrust.variable named "data"
// CHECK-NOT: emitrust.variable named "index"

// The DIRECT member use (`hse->input_size`) subscripts the root slice at
// the root's own cursor — the load the Arm-B path below must mirror.
// CHECK: %[[ENC0:.*]] = emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.slice<!emitrust.struct<"enc">>>, i64) -> !emitrust.lvalue<!emitrust.struct<"enc">>
// CHECK-NEXT: emitrust.member %[[ENC0]]["input_size"] : (!emitrust.lvalue<!emitrust.struct<"enc">>) -> !emitrust.lvalue<ui16>

// Arm B use (read `data[i]`): root slice subscripted at the ROOT'S own
// cursor, then the buffer Vec member, then the local's member-relative
// cursor.
// CHECK: %[[ENC:.*]] = emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.slice<!emitrust.struct<"enc">>>, i64) -> !emitrust.lvalue<!emitrust.struct<"enc">>
// CHECK-NEXT: %[[BUF:.*]] = emitrust.member %[[ENC]]["buffer"] : (!emitrust.lvalue<!emitrust.struct<"enc">>) -> !emitrust.lvalue<!emitrust.opaque<"Vec<u8>">>
// CHECK-NEXT: %[[BYTE:.*]] = emitrust.subscript %[[BUF]][%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<u8>">>, i64) -> !emitrust.lvalue<ui8>
// CHECK-NEXT: emitrust.load %[[BYTE]]

// Arm A use (write `index[i]`): a FRESH full projection chain — root slice
// element, Option member, as_mut().unwrap borrow consumed immediately,
// deref, tail Vec member, subscript — then the assign through it.
// CHECK: %[[ENC2:.*]] = emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.slice<!emitrust.struct<"enc">>>, i64) -> !emitrust.lvalue<!emitrust.struct<"enc">>
// CHECK-NEXT: %[[OPT:.*]] = emitrust.member %[[ENC2]]["search_index"] : (!emitrust.lvalue<!emitrust.struct<"enc">>) -> !emitrust.lvalue<!emitrust.opaque<"Option<hs_index>">>
// CHECK-NEXT: %[[BOR:.*]] = emitrust.method_call %[[OPT]]["as_mut().unwrap"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<hs_index>">>) -> !emitrust.mut_ref<!emitrust.struct<"hs_index">>
// CHECK-NEXT: %[[HSI:.*]] = emitrust.deref %[[BOR]] : (!emitrust.mut_ref<!emitrust.struct<"hs_index">>) -> !emitrust.lvalue<!emitrust.struct<"hs_index">>
// CHECK-NEXT: %[[IDX:.*]] = emitrust.member %[[HSI]]["index"] : (!emitrust.lvalue<!emitrust.struct<"hs_index">>) -> !emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>
// CHECK-NEXT: %[[ELT:.*]] = emitrust.subscript %[[IDX]][%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>, i64) -> !emitrust.lvalue<i16>
// CHECK: emitrust.assign %[[ELT]] = %{{.*}} : !emitrust.lvalue<i16>

int16_t chain_read(struct enc *hse, uint16_t k) {
  struct hs_index *hsi = hse->search_index;
  int16_t *const index = hsi->index;
  int16_t pos = index[k];      /* read through the Arm-A local */
  pos = index[(uint16_t)pos];  /* chain walk: read feeds the next subscript */
  return pos;
}

// Reads through the Arm-A local project just as freshly: two uses, two full
// chains, no borrow held between them (mut_ref root: no slice subscript).
// CHECK-LABEL: func.func @chain_read
// CHECK: emitrust.method_call %{{.*}}["as_mut().unwrap"] ()
// CHECK: %[[R1:.*]] = emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>, i64) -> !emitrust.lvalue<i16>
// CHECK: emitrust.load %[[R1]]
// CHECK: emitrust.method_call %{{.*}}["as_mut().unwrap"] ()
// CHECK: %[[R2:.*]] = emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>, i64) -> !emitrust.lvalue<i16>
// CHECK: emitrust.load %[[R2]]
