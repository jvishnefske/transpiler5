// RUN: split-file %s %t
// RUN: emitrust-import-c %t/local.c | FileCheck %s --check-prefix=LOCAL
// RUN: emitrust-import-c %t/decomposed.c | FileCheck %s --check-prefix=DECOMP

// FR-87: hosted byte-family <string.h> calls (memset/memcpy/memmove/
// memcmp) accept MEMBER-ARRAY regions — the shapes FR-74/86 already
// admit as slice ARGUMENTS now also resolve as memset/memcpy REGIONS.
// The region argument reuses the member-array interception exactly
// (`emitrust.member` chain over the root place -> `emitrust.slice_of`
// at the cursor), then the FR-72 element rules pick the helper image
// (i8 -> the historical helpers, ui8 -> the `_u8` family), so the
// emitted helper signature always agrees with the call site. Pinned:
// the DOT root, the ARROW root, a NESTED dot chain, the OFFSET cursor
// forms (`&c->V[k]` runtime-pure and constant), the DECOMPOSED
// (null-compared struct pointer, tinycrypt's ctr_prng uninstantiate)
// root, memcmp in VALUE position, and — the u32 subset — a `unsigned
// int` member array as a memset DESTINATION: memset semantics are
// bytes, and when the byte count is a compile-time constant multiple
// of 4 and the fill byte is a compile-time constant, the word fill
// `b * 0x01010101` is byte-exact (endianness-neutral: all four bytes
// equal), lowered through the new `__emitrust_memset_u32` image.
// SAME-(root, field-path) memcpy/memmove take ONE mutable borrow of
// the member plus both cursors through `copy_within` (memmove's
// overlap-correct semantics, refining C's undefined overlapping
// memcpy) — the naive two-borrow form is rustc E0502; DISJOINT sibling
// fields of one struct keep the legal two-borrow form. memcmp borrows
// shared, so even the same field twice is admitted.

//--- local.c
#include <string.h>

struct K {
  unsigned int words[8];
};
struct C {
  unsigned char V[16];
  unsigned char W[16];
  struct K key;
  unsigned int n;
};
struct T {
  char name[8];
  unsigned int n;
};
struct Inner {
  unsigned char iv[8];
  unsigned int t;
};
struct Outer {
  struct Inner in;
  unsigned int n;
};

// LOCAL-LABEL: func.func @touch(
// LOCAL-SAME: !emitrust.mut_ref<!emitrust.struct<"C">>
static void touch(struct C *c, unsigned int k) {
  /* ARROW root: memset over the whole ui8 member array. */
  // LOCAL: %[[AV:.+]] = emitrust.member %{{.+}}["V"] : (!emitrust.lvalue<!emitrust.struct<"C">>) -> !emitrust.lvalue<!emitrust.array<16xui8>>
  // LOCAL-NEXT: %[[AC:.+]] = arith.constant 0 : i64
  // LOCAL: %[[AS:.+]] = emitrust.slice_of mut %[[AV]][%[[AC]]] : (!emitrust.lvalue<!emitrust.array<16xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
  // LOCAL: emitrust.call_opaque "__emitrust_memset_u8"(%[[AS]], %{{.+}}, %{{.+}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, i32, i64) -> ()
  memset(c->V, 0xff, sizeof c->V);
  /* OFFSET cursor, runtime PURE index (a parameter read): the FR-86
     cursor form as a REGION — member place at the cast-to-i64 index. */
  // LOCAL: %[[OV:.+]] = emitrust.member %{{.+}}["V"]
  // LOCAL: %[[OC:.+]] = emitrust.cast %{{.+}} : ui32 to i64
  // LOCAL: %[[OS:.+]] = emitrust.slice_of mut %[[OV]][%[[OC]]]
  // LOCAL: emitrust.call_opaque "__emitrust_memset_u8"(%[[OS]],
  memset(&c->V[k], 0xAB, 4);
  /* u32 NESTED member array as memset destination, zero fill, sizeof
     count — the ctr_prng shape: word image, byte count stays i64. */
  // LOCAL: %[[KK:.+]] = emitrust.member %{{.+}}["key"] : (!emitrust.lvalue<!emitrust.struct<"C">>) -> !emitrust.lvalue<!emitrust.struct<"K">>
  // LOCAL-NEXT: %[[KW:.+]] = emitrust.member %[[KK]]["words"] : (!emitrust.lvalue<!emitrust.struct<"K">>) -> !emitrust.lvalue<!emitrust.array<8xui32>>
  // LOCAL: %[[ZS:.+]] = emitrust.slice_of mut %[[KW]][%{{.+}}] : (!emitrust.lvalue<!emitrust.array<8xui32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui32>>
  // LOCAL: %[[ZW:.+]] = emitrust.constant <0 : ui32> : ui32
  // LOCAL: emitrust.call_opaque "__emitrust_memset_u32"(%[[ZS]], %[[ZW]], %{{.+}}) : (!emitrust.mut_ref<!emitrust.slice<ui32>>, ui32, i64) -> ()
  memset(c->key.words, 0x00, sizeof c->key.words);
  /* u32 NONZERO fill: the word MUST be the replicated byte
     0xAB * 0x01010101 = 0xABABABAB (a hand-picked word is exactly the
     miscompile the spike's byte-diff caught). Partial count 8 fills
     two words. */
  // LOCAL: %[[FW:.+]] = emitrust.constant <2880154539 : ui32> : ui32
  // LOCAL: emitrust.call_opaque "__emitrust_memset_u32"(%{{.+}}, %[[FW]], %{{.+}})
  memset(c->key.words, 0xAB, 8);
  /* SAME (root, field-path) memcpy: one mutable member borrow, both
     cursors, `copy_within` (u8 image — the region is ui8). */
  // LOCAL: %[[WV:.+]] = emitrust.member %{{.+}}["V"]
  // LOCAL: %[[WS:.+]] = emitrust.slice_of mut %[[WV]][%{{.+}}] : (!emitrust.lvalue<!emitrust.array<16xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
  // LOCAL: emitrust.call_opaque "__emitrust_memcpy_within_u8"(%[[WS]], %{{.+}}, %{{.+}}, %{{.+}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, i64, i64, i64) -> ()
  memcpy(c->V, c->V + 8, 8);
  /* DISJOINT sibling fields: the legal two-borrow form (mut dst,
     shared src), u8 helper. Both member places resolve at argument
     time; the two borrows are created back to back before the call. */
  // LOCAL: %[[DW:.+]] = emitrust.member %{{.+}}["W"]
  // LOCAL: %[[SV:.+]] = emitrust.member %{{.+}}["V"]
  // LOCAL: %[[DS:.+]] = emitrust.slice_of mut %[[DW]][%{{.+}}]
  // LOCAL-NEXT: %[[SS:.+]] = emitrust.slice_of %[[SV]][%{{.+}}] : (!emitrust.lvalue<!emitrust.array<16xui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
  // LOCAL-NEXT: emitrust.call_opaque "__emitrust_memcpy_u8"(%[[DS]], %[[SS]], %{{.+}})
  memcpy(c->W, c->V, sizeof c->W);
  /* Overlapping same-field memmove: the same copy_within lowering is
     exactly memmove's semantics; dst cursor is the runtime index. */
  // LOCAL: emitrust.call_opaque "__emitrust_memcpy_within_u8"(
  memmove(&c->W[k], c->W, 8);
}

// LOCAL-LABEL: func.func @c_main
int main(void) {
  struct C c;
  struct T t;
  struct Outer o;
  /* DOT root over the local struct. */
  // LOCAL: %[[MV:.+]] = emitrust.member %{{.+}}["V"] : (!emitrust.lvalue<!emitrust.struct<"C">>) -> !emitrust.lvalue<!emitrust.array<16xui8>>
  // LOCAL: %[[MS:.+]] = emitrust.slice_of mut %[[MV]][%{{.+}}]
  // LOCAL: emitrust.call_opaque "__emitrust_memset_u8"(%[[MS]],
  memset(c.V, 0, sizeof c.V);
  /* An i8 (char) member array keeps the historical i8 helper: the u8
     image is selected by the region element, never blanket-applied. */
  // LOCAL: %[[TN:.+]] = emitrust.member %{{.+}}["name"] : (!emitrust.lvalue<!emitrust.struct<"T">>) -> !emitrust.lvalue<!emitrust.array<8xi8>>
  // LOCAL: %[[TS:.+]] = emitrust.slice_of mut %[[TN]][%{{.+}}]
  // LOCAL: emitrust.call_opaque "__emitrust_memset"(%[[TS]], %{{.+}}, %{{.+}}) : (!emitrust.mut_ref<!emitrust.slice<i8>>, i32, i64) -> ()
  memset(t.name, 'x', 3);
  /* NESTED dot chain: two member projections, then the slice. */
  // LOCAL: %[[NI:.+]] = emitrust.member %{{.+}}["in_"] : (!emitrust.lvalue<!emitrust.struct<"Outer">>) -> !emitrust.lvalue<!emitrust.struct<"Inner">>
  // LOCAL-NEXT: %[[NIV:.+]] = emitrust.member %[[NI]]["iv"] : (!emitrust.lvalue<!emitrust.struct<"Inner">>) -> !emitrust.lvalue<!emitrust.array<8xui8>>
  // LOCAL: %[[NS:.+]] = emitrust.slice_of mut %[[NIV]][%{{.+}}]
  // LOCAL: emitrust.call_opaque "__emitrust_memset_u8"(%[[NS]],
  memset(o.in.iv, 1, sizeof o.in.iv);
  t.n = 0u;
  o.n = 0u;
  c.n = 0u;
  touch(&c, 2u);
  /* memcmp in VALUE position: both borrows shared, sibling fields. */
  // LOCAL: %[[LV:.+]] = emitrust.member %{{.+}}["V"]
  // LOCAL: %[[RW:.+]] = emitrust.member %{{.+}}["W"]
  // LOCAL: %[[LS:.+]] = emitrust.slice_of %[[LV]][%{{.+}}] : (!emitrust.lvalue<!emitrust.array<16xui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
  // LOCAL-NEXT: %[[RS:.+]] = emitrust.slice_of %[[RW]][%{{.+}}]
  // LOCAL-NEXT: emitrust.call_opaque "__emitrust_memcmp_u8"(%[[LS]], %[[RS]], %{{.+}}) : (!emitrust.ref<!emitrust.slice<ui8>>, !emitrust.ref<!emitrust.slice<ui8>>, i64) -> i32
  return memcmp(c.V, c.W, 8) & 1;
}

// Every requested helper image is emitted exactly once, as safe Rust;
// the u32 fill image walks words but compares BYTE count.
// LOCAL: emitrust.verbatim "fn __emitrust_memset(s: &mut [i8], c: i32, n: i64)
// LOCAL: emitrust.verbatim "fn __emitrust_memset_u8(s: &mut [u8], c: i32, n: i64)
// LOCAL: emitrust.verbatim "fn __emitrust_memcpy_u8(dst: &mut [u8], src: &[u8], n: i64)
// LOCAL: emitrust.verbatim "fn __emitrust_memcmp_u8(a: &[u8], b: &[u8], n: i64) -> i32
// LOCAL: emitrust.verbatim "fn __emitrust_memcpy_within_u8(s: &mut [u8], dst: i64, src: i64, n: i64)
// LOCAL: emitrust.verbatim "fn __emitrust_memset_u32(s: &mut [u32], w: u32, n: i64)
// LOCAL-NOT: unsafe

//--- decomposed.c

/* Library TU (no main, external linkage): the null-compared context
   pointer demotes to the FR-86 DECOMPOSED slice-of-struct
   representation — tinycrypt ctr_prng's tc_ctr_prng_uninstantiate,
   verbatim: the u32 nested `ctx->key.words` memset (line 275) and the
   byte `ctx->V` memset (line 276), both with sizeof counts. */

#include <string.h>

struct K {
  unsigned int words[44];
};
struct P {
  struct K key;
  unsigned char V[16];
  unsigned int reseedCount;
};

// DECOMP-LABEL: func.func @uninst(
// DECOMP-SAME: !emitrust.mut_ref<!emitrust.slice<!emitrust.struct<"P">>>
void uninst(struct P *ctx) {
  if (0 != ctx) {
    /* subscript-at-cursor -> member -> member -> mut u32 slice ->
       word-fill image; sizeof count 44*4 = 176 stays the byte count. */
    // DECOMP: %[[UP:.+]] = emitrust.subscript %{{.+}}[%{{.+}}] : (!emitrust.lvalue<!emitrust.slice<!emitrust.struct<"P">>>, i64) -> !emitrust.lvalue<!emitrust.struct<"P">>
    // DECOMP-NEXT: %[[UK:.+]] = emitrust.member %[[UP]]["key"]
    // DECOMP-NEXT: %[[UW:.+]] = emitrust.member %[[UK]]["words"] : (!emitrust.lvalue<!emitrust.struct<"K">>) -> !emitrust.lvalue<!emitrust.array<44xui32>>
    // DECOMP: %[[US:.+]] = emitrust.slice_of mut %[[UW]][%{{.+}}] : (!emitrust.lvalue<!emitrust.array<44xui32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui32>>
    // DECOMP: %[[UZ:.+]] = emitrust.constant <0 : ui32> : ui32
    // DECOMP: emitrust.call_opaque "__emitrust_memset_u32"(%[[US]], %[[UZ]], %{{.+}})
    memset(ctx->key.words, 0x00, sizeof ctx->key.words);
    /* The byte member of the same decomposed root: u8 image. */
    // DECOMP: %[[VP:.+]] = emitrust.subscript
    // DECOMP-NEXT: %[[VM:.+]] = emitrust.member %[[VP]]["V"] : (!emitrust.lvalue<!emitrust.struct<"P">>) -> !emitrust.lvalue<!emitrust.array<16xui8>>
    // DECOMP: %[[VS:.+]] = emitrust.slice_of mut %[[VM]][%{{.+}}]
    // DECOMP: emitrust.call_opaque "__emitrust_memset_u8"(%[[VS]],
    memset(ctx->V, 0x00, sizeof ctx->V);
    ctx->reseedCount = 0u;
  }
}
// DECOMP: emitrust.verbatim "fn __emitrust_memset_u32(s: &mut [u32], w: u32, n: i64)
// DECOMP-NOT: unsafe
