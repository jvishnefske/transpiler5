// RUN: split-file %s %t
// RUN: emitrust-import-c %t/param.c | FileCheck %s --check-prefix=PARAM --implicit-check-not=emitrust.struct_def
// RUN: emitrust-import-c %t/local.c | FileCheck %s --check-prefix=LOCAL --implicit-check-not=emitrust.struct_def

// FR-91: member-array arguments and byte-family regions on a
// BYTE-REGION root (CTS-BR: every scalar leaf `unsigned char`) ride
// the byte-region machinery instead of rejecting at the decay. A
// byte-region record has no member PLACES — the object is one flat
// `!emitrust.array<Nxui8>` region and a `struct *` parameter over it
// is a `&[u8]`/`&mut [u8]` slice — so the FR-74/86 typed member
// matcher rightly declines its chains; historically that dropped the
// tiny-AES-c shapes (`KeyExpansion(ctx->RoundKey, key)`,
// `memcpy(ctx->Iv, iv, n)`, `memcpy(buffer, ctx->Iv, n)`, aes.c
// 221/226/231/549) into the located ArrayToPointerDecay rejection.
// The byte-region model already expresses the argument exactly: the
// member is the region base at its constant layout byte offset
// (`resolveByteRegionRef`), and `emitrust.slice_of` takes a START
// cursor, so the argument is an OPEN-ENDED window
// `slice_of [mut] %region[cursor + constOff]`. Over-wide windows are
// faithful because every byte-family helper carries an explicit count
// and slice-param callees never read a length. Aliasing keys on the
// ROOT with an EMPTY field path: windows of one region share ONE
// slice place, so a second window of the same root in one call
// collides (two `slice_of`s of one place with a mut is rustc E0502 —
// the FR-74 disjoint-sibling two-borrow admission does NOT transfer
// to byte-region roots), while the byte-family same-root pair rides
// `copy_within` on the whole region with ABSOLUTE byte cursors
// (memmove's overlap-correct semantics). This deliberately MOVES the
// FR-87/FR-89-pinned all-u8 boundary forward: the single-window
// shapes flip from the decay rejection to working code (see
// byte-region-member-window-invalid.c for what stays located).

//--- param.c
// ARROW root through a byte-region `struct *` parameter — the aes.c
// shapes verbatim. The param is the deref'd `lvalue<slice<ui8>>` with
// an FR-71 runtime byte cursor; every window is `cursor + constOff`.
#include <string.h>
typedef unsigned char u8;
struct Ctx { u8 RoundKey[176]; u8 Iv[16]; };

// PARAM-LABEL: func.func @expand(
// PARAM-SAME: !emitrust.mut_ref<!emitrust.slice<ui8>>, %{{.+}}: !emitrust.ref<!emitrust.slice<ui8>>
static void expand(u8 *rk, const u8 *key) { rk[0] = key[0]; }

// Slice ARGUMENT position (KeyExpansion): a mut window at region
// offset 0 — the cursor load alone (byteRegionOffset folds `+ 0`).
// PARAM-LABEL: func.func @init(
// PARAM-SAME: %[[CTX:.+]]: !emitrust.mut_ref<!emitrust.slice<ui8>>
// PARAM: %[[CP:.+]] = emitrust.deref %[[CTX]] : (!emitrust.mut_ref<!emitrust.slice<ui8>>) -> !emitrust.lvalue<!emitrust.slice<ui8>>
// PARAM: %[[IC:.+]] = memref.load
// PARAM: %[[IW:.+]] = emitrust.slice_of mut %[[CP]][%[[IC]]] : (!emitrust.lvalue<!emitrust.slice<ui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// PARAM: call @expand(%[[IW]],
void init(struct Ctx *ctx, const u8 *key) {
  expand(ctx->RoundKey, key);
}

// Byte-family DST window (memcpy dst `ctx->Iv`): region offset 176,
// u8 helper image; the src is the ordinary shared slice param region.
// PARAM-LABEL: func.func @set_iv(
// PARAM: %[[SC:.+]] = memref.load
// PARAM: %[[SO:.+]] = arith.constant 176 : i64
// PARAM: %[[SA:.+]] = arith.addi %[[SO]], %[[SC]] : i64
// PARAM: %[[SW:.+]] = emitrust.slice_of mut %{{.+}}[%[[SA]]] : (!emitrust.lvalue<!emitrust.slice<ui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// PARAM: emitrust.call_opaque "__emitrust_memcpy_u8"(%[[SW]], %{{.+}}, %{{.+}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, !emitrust.ref<!emitrust.slice<ui8>>, i64) -> ()
void set_iv(struct Ctx *ctx, const u8 *iv) {
  memcpy(ctx->Iv, iv, 16);
}

// SHARED SRC window through a CONST byte-region param (aes.c:549:
// `memcpy(buffer, ctx->Iv, n)`): the window borrow is shared — a mut
// window through a shared region is declined (see the invalid file).
// PARAM-LABEL: func.func @out_iv(
// PARAM-SAME: %[[OC:.+]]: !emitrust.ref<!emitrust.slice<ui8>>
// PARAM: %[[OW:.+]] = emitrust.slice_of %{{.+}}[%{{.+}}] : (!emitrust.lvalue<!emitrust.slice<ui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
// PARAM: emitrust.call_opaque "__emitrust_memcpy_u8"(%{{.+}}, %[[OW]], %{{.+}})
void out_iv(const struct Ctx *ctx, u8 *buffer) {
  memcpy(buffer, ctx->Iv, 16);
}

// memset over a window: the u8 fill image.
// PARAM-LABEL: func.func @wipe(
// PARAM: %[[WW:.+]] = emitrust.slice_of mut %{{.+}}[%{{.+}}]
// PARAM: emitrust.call_opaque "__emitrust_memset_u8"(%[[WW]], %{{.+}}, %{{.+}})
void wipe(struct Ctx *ctx) {
  memset(ctx->Iv, 0, 16);
}

// Same-root memmove with RUNTIME offsets (`&ctx->Iv[0]`,
// `&ctx->Iv[k]`): ONE whole-region mut borrow at cursor 0 plus both
// ABSOLUTE byte cursors through `copy_within` — the FR-87 image, only
// the window base differs.
// PARAM-LABEL: func.func @shift(
// PARAM: %[[MC1:.+]] = arith.constant 176 : i64
// PARAM-NEXT: %[[MD:.+]] = arith.addi %[[MC1]], %{{.+}} : i64
// PARAM: %[[MC2:.+]] = arith.constant 176 : i64
// PARAM-NEXT: %[[MS0:.+]] = arith.addi %[[MC2]], %{{.+}} : i64
// PARAM: %[[MSRC:.+]] = arith.addi %[[MS0]], %{{.+}} : i64
// PARAM: %[[MZ:.+]] = arith.constant 0 : i64
// PARAM-NEXT: %[[MW:.+]] = emitrust.slice_of mut %{{.+}}[%[[MZ]]] : (!emitrust.lvalue<!emitrust.slice<ui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// PARAM-NEXT: emitrust.call_opaque "__emitrust_memcpy_within_u8"(%[[MW]], %[[MD]], %[[MSRC]], %{{.+}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, i64, i64, i64) -> ()
void shift(struct Ctx *ctx, unsigned k, unsigned n) {
  memmove(&ctx->Iv[0], &ctx->Iv[k], n);
}

// Same-root CROSS-FIELD memcpy (`ctx->Iv`, `ctx->RoundKey`): the two
// windows are views of ONE slice place, so the two-borrow form is
// E0502 — the pair rides the same whole-region `copy_within` with the
// fields' absolute region offsets (176 and 0). memmove semantics
// refine C's undefined overlapping memcpy; here the fields are
// disjoint by layout, so the copy is byte-exact.
// PARAM-LABEL: func.func @mix(
// PARAM: emitrust.call_opaque "__emitrust_memcpy_within_u8"(
void mix(struct Ctx *ctx) {
  memcpy(ctx->Iv, ctx->RoundKey, 16);
}

// The helper images arrive as safe Rust.
// PARAM: emitrust.verbatim "fn __emitrust_memset_u8(s: &mut [u8], c: i32, n: i64)
// PARAM: emitrust.verbatim "fn __emitrust_memcpy_u8(dst: &mut [u8], src: &[u8], n: i64)
// PARAM: emitrust.verbatim "fn __emitrust_memcpy_within_u8(s: &mut [u8], dst: i64, src: i64, n: i64)
// PARAM-NOT: unsafe

//--- local.c
// DOT root over a LOCAL byte-region struct: the region place is the
// flat byte array itself, windows are constant offsets into it.
#include <string.h>
typedef unsigned char u8;
struct B { u8 a[8]; u8 b[8]; };

// LOCAL-LABEL: func.func @win(
static void win(u8 *p) { p[0] = 1; }

// LOCAL-LABEL: func.func @c_main
int main(void) {
  struct B s;
  // memset window at offset 0 over the 16-byte region.
  // LOCAL: %[[MP:.+]] = emitrust.slice_of mut %{{.+}}[%{{.+}}] : (!emitrust.lvalue<!emitrust.array<16xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
  // LOCAL: emitrust.call_opaque "__emitrust_memset_u8"(%[[MP]], %{{.+}}, %{{.+}})
  memset(s.a, 1, 8);
  // Slice ARGUMENT window of the second field: constant offset 8.
  // LOCAL: %[[AC:.+]] = arith.constant 8 : i64
  // LOCAL: %[[AW:.+]] = emitrust.slice_of mut %{{.+}}[%[[AC]]] : (!emitrust.lvalue<!emitrust.array<16xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
  // LOCAL: call @win(%[[AW]])
  win(s.b);
  // OFFSET spelling `s.a + 2` folds into the window cursor: 0 + 2.
  // LOCAL: %[[OC:.+]] = arith.constant 2 : i64
  // LOCAL: %[[OW:.+]] = emitrust.slice_of mut %{{.+}}[%[[OC]]]
  // LOCAL: emitrust.call_opaque "__emitrust_memset_u8"(%[[OW]],
  memset(s.a + 2, 0, 4);
  // Same-root cross-field memcpy: whole-region `copy_within`,
  // absolute cursors 8 (dst field) and 0 (src field).
  // LOCAL: %[[WS:.+]] = emitrust.slice_of mut %{{.+}}[%{{.+}}]
  // LOCAL: emitrust.call_opaque "__emitrust_memcpy_within_u8"(%[[WS]], %{{.+}}, %{{.+}}, %{{.+}})
  memcpy(s.b, s.a, 8);
  // memcmp: both windows SHARED — two shared views of one place
  // coexist.
  // LOCAL: %[[CL:.+]] = emitrust.slice_of %{{.+}}[%{{.+}}] : (!emitrust.lvalue<!emitrust.array<16xui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
  // LOCAL: %[[CR:.+]] = emitrust.slice_of %{{.+}}[%{{.+}}] : (!emitrust.lvalue<!emitrust.array<16xui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
  // LOCAL: emitrust.call_opaque "__emitrust_memcmp_u8"(%[[CL]], %[[CR]], %{{.+}})
  return memcmp(s.a, s.b, 8) & 1;
}
// LOCAL: emitrust.verbatim "fn __emitrust_memcpy_within_u8(s: &mut [u8], dst: i64, src: i64, n: i64)
// LOCAL-NOT: unsafe
