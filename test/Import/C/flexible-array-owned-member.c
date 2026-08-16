// RUN: emitrust-import-c %s | FileCheck %s

// FR-96: a struct FIELD holding a pointer to an ADMITTED FAM record —
// heatshrink_encoder's `struct hs_index *search_index` (heatshrink_encoder.h:67)
// inside a container that is ITSELF an admitted FAM record — becomes an OWNED
// NULLABLE member: `!emitrust.opaque<"Option<hs_index>">`, composing FR-94/95's
// owned value type with the FR-88 Option machinery (opaque Option type,
// `is_some`/`is_none` method calls, `Some` via call_opaque, the `None`
// literal). What this file pins:
//   1. The field declaration: the pointer-to-admitted-FAM-record field of a
//      FAM container types as the opaque Option of the pointee's struct
//      symbol (a NON-FAM container keeps the historical i64 slot — see the
//      invalid file's scope arms).
//   2. Alloc-into-member (encoder.c:92-98): `hse->search_index =
//      malloc(index_sz + sizeof(struct hs_index))` — the size-local peel and
//      the FR-95 multiply-form element-count extraction ride along — lowers
//      to a temp `hs_index` value whose tail binds `vec![0i16; n]`, wrapped
//      `Some(...)` and assigned to the member place. The malloc-failure
//      guard is NOT elided (constraint: no isNullTestOf widening): it lowers
//      FAITHFULLY to an `is_none` branch, infallibly false at runtime.
//   3. Member-of-member access (`hse->search_index->size`, encoder.c:98/109)
//      and the member-read local (`struct hs_index *hsi = hse->search_index`,
//      encoder.c:420/463) both lower as PER-USE projections —
//      `method_call ["as_mut().unwrap"] -> mut_ref -> deref` — never a
//      let-bound borrow (the let-bound shape is rustc E0499 against the real
//      do_indexing structure; measured in the FR-96 spike). The member-read
//      local itself binds NO code.
//   4. Free-of-member (`free(hse->search_index)`, encoder.c:110) assigns the
//      `None` literal — the drop IS the deallocation — and composes with the
//      FR-94 free-only wrapper claim (member reads + frees only), whose own
//      `free(hse)` stays a no-op drop of the OWNED-BY-VALUE parameter.
//   5. Null tests in both polarities: `== NULL` -> `is_none`, `!= NULL` ->
//      `is_some` (FR-88's let-bound i1 shape); `hse->search_index = NULL`
//      assigns `None`.
// The frontier (address-of the member, ++/--, escaping the member-read
// local, unrecognized alloc forms, self-referential and non-FAM containers)
// stays in flexible-array-owned-member-invalid.c.

#include <stdlib.h>

struct hs_index {
  unsigned short size;
  short index[];
};

struct enc {
  unsigned short input_size;
  struct hs_index *search_index;
  unsigned char buffer[];
};

// Pin 1: the field declaration — the lifted member types as the opaque
// Option of the pointee record's struct symbol; the container's own FAM
// tail keeps its FR-94 Vec field.
// CHECK: emitrust.struct_def @hs_index ["size", "index"] [ui16, !emitrust.opaque<"Vec<i16>">]
// CHECK: emitrust.struct_def @enc ["input_size", "search_index", "buffer"] [ui16, !emitrust.opaque<"Option<hs_index>">, !emitrust.opaque<"Vec<u8>">]

void enc_index_init(struct enc *hse, unsigned short n) {
  size_t index_sz = n * sizeof(unsigned short); /* numeric-fold factor */
  hse->search_index = malloc(index_sz + sizeof(struct hs_index));
  if (hse->search_index == NULL) {
    return;
  }
  hse->search_index->size = (unsigned short)index_sz;
}

// Pin 2: Some-assignment from the recognized alloc — temp hs_index value,
// element-count vec_fill (the ui16 param widened through the size_t multiply,
// exactly FR-95's count arithmetic), Some(...) moved into the member place.
// CHECK-LABEL: func.func @enc_index_init
// CHECK: %[[OPT:.*]] = emitrust.member %{{.*}}["search_index"] : (!emitrust.lvalue<!emitrust.struct<"enc">>) -> !emitrust.lvalue<!emitrust.opaque<"Option<hs_index>">>
// CHECK: %[[TMP:.*]] = emitrust.variable {{.*}}: !emitrust.lvalue<!emitrust.struct<"hs_index">>
// CHECK: %[[TAIL:.*]] = emitrust.member %[[TMP]]["index"] : (!emitrust.lvalue<!emitrust.struct<"hs_index">>) -> !emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>
// CHECK: %[[CNT:.*]] = emitrust.cast %{{.*}} : ui64 to i64
// CHECK-NEXT: %[[FILL:.*]] = emitrust.vec_fill "0i16", %[[CNT]] : i64 -> <"Vec<i16>">
// CHECK-NEXT: emitrust.assign %[[TAIL]] = %[[FILL]] : !emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>
// CHECK: %[[VAL:.*]] = emitrust.load %[[TMP]] : (!emitrust.lvalue<!emitrust.struct<"hs_index">>) -> !emitrust.struct<"hs_index">
// CHECK-NEXT: %[[SOME:.*]] = emitrust.call_opaque "Some"(%[[VAL]]) : (!emitrust.struct<"hs_index">) -> !emitrust.opaque<"Option<hs_index>">
// CHECK-NEXT: emitrust.assign %[[OPT]] = %[[SOME]] : !emitrust.lvalue<!emitrust.opaque<"Option<hs_index>">>
// The malloc-failure guard stays FAITHFUL — an is_none branch, never elided.
// CHECK: %[[GUARD:.*]] = emitrust.method_call %{{.*}}["is_none"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<hs_index>">>) -> i1
// CHECK: cf.cond_br %[[GUARD]]
// The member-of-member write projects the payload per use.
// CHECK: %[[BOR:.*]] = emitrust.method_call %{{.*}}["as_mut().unwrap"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<hs_index>">>) -> !emitrust.mut_ref<!emitrust.struct<"hs_index">>
// CHECK-NEXT: %[[HSI:.*]] = emitrust.deref %[[BOR]] : (!emitrust.mut_ref<!emitrust.struct<"hs_index">>) -> !emitrust.lvalue<!emitrust.struct<"hs_index">>
// CHECK: emitrust.member %[[HSI]]["size"] : (!emitrust.lvalue<!emitrust.struct<"hs_index">>) -> !emitrust.lvalue<ui16>

short enc_index_get(struct enc *hse, unsigned short k) {
  struct hs_index *hsi = hse->search_index; /* member-read local: NO binding */
  short v = hsi->index[k];                  /* projected element indexing */
  unsigned char b = hse->buffer[k];         /* container tail, same scope */
  return (short)(v + b);
}

// Pin 3: the member-read local binds nothing; each use is a fresh projection
// (member -> as_mut().unwrap -> deref), so no long-lived borrow ever exists
// alongside the container's OWN tail use in the same scope.
// CHECK-LABEL: func.func @enc_index_get
// CHECK-NOT: emitrust.variable named "hsi"
// CHECK: %[[GOPT:.*]] = emitrust.member %{{.*}}["search_index"] : (!emitrust.lvalue<!emitrust.struct<"enc">>) -> !emitrust.lvalue<!emitrust.opaque<"Option<hs_index>">>
// CHECK-NEXT: %[[GBOR:.*]] = emitrust.method_call %[[GOPT]]["as_mut().unwrap"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<hs_index>">>) -> !emitrust.mut_ref<!emitrust.struct<"hs_index">>
// CHECK-NEXT: %[[GHSI:.*]] = emitrust.deref %[[GBOR]] : (!emitrust.mut_ref<!emitrust.struct<"hs_index">>) -> !emitrust.lvalue<!emitrust.struct<"hs_index">>
// CHECK-NEXT: %[[GIDX:.*]] = emitrust.member %[[GHSI]]["index"] : (!emitrust.lvalue<!emitrust.struct<"hs_index">>) -> !emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>
// CHECK: emitrust.subscript %[[GIDX]][%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<i16>">>, ui16) -> !emitrust.lvalue<i16>
// The container's own Vec tail read stays the ordinary FR-94 member place.
// CHECK: emitrust.member %{{.*}}["buffer"] : (!emitrust.lvalue<!emitrust.struct<"enc">>) -> !emitrust.lvalue<!emitrust.opaque<"Vec<u8>">>

int enc_has_index(struct enc *hse) {
  if (hse->search_index != NULL) {
    return 1;
  }
  if (hse->search_index == NULL) {
    return 0;
  }
  return 2;
}

// Pin 5a: both null-test polarities are REAL (None is the unallocated/freed
// state) — never a statically-non-null fold.
// CHECK-LABEL: func.func @enc_has_index
// CHECK: %[[S:.*]] = emitrust.method_call %{{.*}}["is_some"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<hs_index>">>) -> i1
// CHECK: cf.cond_br %[[S]]
// CHECK: %[[NN:.*]] = emitrust.method_call %{{.*}}["is_none"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<hs_index>">>) -> i1
// CHECK: cf.cond_br %[[NN]]

void enc_drop_index(struct enc *hse) {
  hse->input_size = 0;
  hse->search_index = NULL;
}

// Pin 5b: NULL-assignment is the None literal on the member place.
// CHECK-LABEL: func.func @enc_drop_index
// CHECK: %[[NONE:.*]] = emitrust.literal "None" : !emitrust.opaque<"Option<hs_index>">
// CHECK-NEXT: emitrust.assign %{{.*}} = %[[NONE]] : !emitrust.lvalue<!emitrust.opaque<"Option<hs_index>">>

void enc_index_free(struct enc *hse) {
  size_t index_sz =
      sizeof(struct hs_index) + hse->search_index->size; /* projected read */
  free(hse->search_index); /* None-assignment: the drop deallocates */
  free(hse);               /* FR-94 owned free wrapper: no-op drop */
  (void)index_sz;
}

// Pin 4: the heatshrink_encoder_free shape (encoder.c:107-113). The
// parameter stays the FR-94 OWNED-BY-VALUE free wrapper (member reads and
// frees only), the member-of-member read projects per use, free-of-member
// assigns None, and free of the parameter itself emits nothing.
// CHECK-LABEL: func.func @enc_index_free
// CHECK-SAME: (%{{.*}}: !emitrust.struct<"enc">)
// CHECK: emitrust.method_call %{{.*}}["as_mut().unwrap"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<hs_index>">>) -> !emitrust.mut_ref<!emitrust.struct<"hs_index">>
// CHECK: emitrust.member %{{.*}}["size"] : (!emitrust.lvalue<!emitrust.struct<"hs_index">>) -> !emitrust.lvalue<ui16>
// CHECK: %[[FNONE:.*]] = emitrust.literal "None" : !emitrust.opaque<"Option<hs_index>">
// CHECK-NEXT: emitrust.assign %{{.*}} = %[[FNONE]] : !emitrust.lvalue<!emitrust.opaque<"Option<hs_index>">>
// CHECK-NOT: func.call @free
// CHECK: return
