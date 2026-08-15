// RUN: emitrust-import-c %s | FileCheck %s

// FR-95: a flexible-array-member record with a TYPED (non-u8) tail —
// heatshrink's second FAM record, `struct hs_index { uint16_t size;
// int16_t index[]; }` (heatshrink_encoder.h:35-38). The FR-94 owned-tail
// admission generalizes: a gap-free struct whose FAM leaf is any FR-65
// Vec-mappable scalar (i16/i32/i64/u16/u32/u64/f32/f64) grows a trailing
// owned `Vec<T>` field, and the access shapes lower over it exactly like
// the byte tail. What THIS file pins beyond FR-94:
//   1. The allocation count is the MULTIPLY form: the non-sizeof side of
//      `malloc(k*sizeof(elem) + sizeof(S))` must divide by the element
//      size, so the recorded count is the extracted ELEMENT count `k`
//      (`vec![0i16; k]`), never the byte extent. The sizeof factor matches
//      NUMERICALLY — heatshrink spells `buf_sz * sizeof(uint16_t)` over an
//      int16_t tail (both 2 bytes), so a syntactic type match would miss
//      the corpus.
//   2. The count side may route through its own never-rewritten local
//      (heatshrink's `size_t index_sz = buf_sz*sizeof(uint16_t);
//      malloc(index_sz + sizeof(struct hs_index))` spelling, encoder.c:92-94).
//   3. `h->index[i]` is element-typed member + subscript (lvalue<i16>);
//      stores of negative values prove the typing (a byte tail cannot).
//   4. The bare tail decay `int16_t *const index = hsi->index`
//      (encoder.c:425) and the `&h->index[k]` form bind cursored pointer
//      locals over the Vec member; the pointee must equal the element
//      EXACTLY (the void wildcard stays u8-only — see the invalid file).
//   5. The element-agnostic FR-94 machinery rides along unchanged: owned
//      struct return, elided null guards, no-op free of a claimed local,
//      free-only wrapper params OWNED BY VALUE.
// The frontier arms (mismatched arithmetic, plain-add typed form, byte
// views, gap layouts, non-Vec elements) stay in
// flexible-array-owned-tail-invalid.c.

#include <stdlib.h>

struct hs_index {
  unsigned short size;
  short index[];
};

// The struct_def grows the owned TYPED tail field at the FAM's position.
// CHECK: emitrust.struct_def @hs_index ["size", "index"] [ui16, !emitrust.opaque<"Vec<i16>">]

struct hs_index *idx_alloc(unsigned short n) {
  size_t index_sz = n * sizeof(unsigned short); /* numeric-fold factor */
  struct hs_index *hsi = malloc(index_sz + sizeof(struct hs_index));
  if (hsi == NULL) return NULL;
  hsi->size = (unsigned short)index_sz; /* BYTE count stays byte-valued */
  return hsi;
}

// Owned struct return; the null guard is elided; the vec_fill count is the
// extracted ELEMENT count `n` (the ui16 parameter widened to i64 — the byte
// extent `index_sz` would arrive as a ui64 multiply instead), and the fill
// literal is the suffixed element zero.
// CHECK-LABEL: func.func @idx_alloc
// CHECK-SAME: -> !emitrust.struct<"hs_index">
// CHECK-NOT: cf.cond_br
// CHECK: %[[TAIL:.*]] = emitrust.member %[[H:.*]]["index"] : (!emitrust.lvalue<!emitrust.struct<"hs_index">>) -> !emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>
// CHECK-NEXT: %[[NL:.*]] = emitrust.load %{{.*}} : (!emitrust.lvalue<ui16>) -> ui16
// CHECK-NEXT: %[[NW:.*]] = emitrust.cast %[[NL]] : ui16 to ui64
// CHECK-NEXT: %[[CNT:.*]] = emitrust.cast %[[NW]] : ui64 to i64
// CHECK-NEXT: %[[FILL:.*]] = emitrust.vec_fill "0i16", %[[CNT]] : i64 -> <"Vec<i16>">
// CHECK-NEXT: emitrust.assign %[[TAIL]] = %[[FILL]] : !emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>
// The byte-count store into the u16 size field is ordinary user code.
// CHECK: emitrust.member %[[H]]["size"] : (!emitrust.lvalue<!emitrust.struct<"hs_index">>) -> !emitrust.lvalue<ui16>

short idx_direct(unsigned short k) {
  struct hs_index *h = malloc(sizeof(struct hs_index) + k * sizeof(short));
  if (!h) return -1;
  h->index[0] = -3;
  short v = h->index[0];
  free(h);
  return v;
}

// The multiply syntactic in the malloc argument (sizeof(S) on the left,
// elem sizeof spelled with the element's own type); `free` of the claimed
// local leaves no trace — the owned struct drops at scope end.
// CHECK-LABEL: func.func @idx_direct
// CHECK-NOT: cf.cond_br
// CHECK: %[[KL:.*]] = emitrust.load %{{.*}} : (!emitrust.lvalue<ui16>) -> ui16
// CHECK-NEXT: %[[KW:.*]] = emitrust.cast %[[KL]] : ui16 to ui64
// CHECK-NEXT: %[[DCNT:.*]] = emitrust.cast %[[KW]] : ui64 to i64
// CHECK-NEXT: emitrust.vec_fill "0i16", %[[DCNT]] : i64 -> <"Vec<i16>">
// CHECK-NOT: func.call @free
// CHECK: return

short idx_get(struct hs_index *h, unsigned short i) {
  return h->index[i];
}

// Element-typed member + subscript through the ordinary `&mut S` parameter
// convention (heatshrink's `pos = hsi->index[pos]` chain-walk reads).
// CHECK-LABEL: func.func @idx_get
// CHECK: %[[VT:.*]] = emitrust.member %{{.*}}["index"] : (!emitrust.lvalue<!emitrust.struct<"hs_index">>) -> !emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>
// CHECK: %[[CELL:.*]] = emitrust.subscript %[[VT]][%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>, ui16) -> !emitrust.lvalue<i16>
// CHECK: emitrust.load %[[CELL]] : (!emitrust.lvalue<i16>) -> i16

short idx_decay(struct hs_index *h, unsigned short k) {
  short *const index = h->index; /* encoder.c:425's bare decay */
  index[k] = (short)(k - 1);
  return index[k];
}

// The bare decay binds a cursored pointer local over the member place; each
// access subscripts the Vec member at the (i64) cursor, element-typed.
// CHECK-LABEL: func.func @idx_decay
// CHECK: emitrust.member %{{.*}}["index"] : (!emitrust.lvalue<!emitrust.struct<"hs_index">>) -> !emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>
// CHECK: %[[WCELL:.*]] = emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>, i64) -> !emitrust.lvalue<i16>
// CHECK: emitrust.assign %[[WCELL]] = %{{.*}} : !emitrust.lvalue<i16>

short idx_window(struct hs_index *h, unsigned short k) {
  short *p = &h->index[k];
  p[0] = -7;
  return p[0];
}

// The `&h->index[k]` spelling of the same binding: cursor starts at k.
// CHECK-LABEL: func.func @idx_window
// CHECK: emitrust.member %{{.*}}["index"] : (!emitrust.lvalue<!emitrust.struct<"hs_index">>) -> !emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>
// CHECK: emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>, i64) -> !emitrust.lvalue<i16>

void idx_free(struct hs_index *h) { free(h); }

// The free-only wrapper takes the record OWNED BY VALUE and emits no free
// call — dropping the parameter IS the deallocation.
// CHECK-LABEL: func.func @idx_free
// CHECK-SAME: (%{{.*}}: !emitrust.struct<"hs_index">)
// CHECK-NOT: call
// CHECK: return

short caller(void) {
  struct hs_index *h = idx_alloc(6);
  if (!h) return -1;
  short v = idx_get(h, 2);
  idx_free(h);
  return v;
}

// The caller binds the owned return to its own struct local, passes it by
// `&mut` to the accessor, and MOVES it into the owned free wrapper.
// CHECK-LABEL: func.func @caller
// CHECK-NOT: cf.cond_br
// CHECK: %[[CH:.*]] = call @idx_alloc(%{{.*}}) : (ui16) -> !emitrust.struct<"hs_index">
// CHECK: emitrust.assign %[[HP:.*]] = %[[CH]] : !emitrust.lvalue<!emitrust.struct<"hs_index">>
// CHECK: emitrust.addr_of mut %[[HP]] : (!emitrust.lvalue<!emitrust.struct<"hs_index">>) -> !emitrust.mut_ref<!emitrust.struct<"hs_index">>
// CHECK: call @idx_get
// CHECK: %[[MV:.*]] = emitrust.load %[[HP]] : (!emitrust.lvalue<!emitrust.struct<"hs_index">>) -> !emitrust.struct<"hs_index">
// CHECK: call @idx_free(%[[MV]]) : (!emitrust.struct<"hs_index">) -> ()
