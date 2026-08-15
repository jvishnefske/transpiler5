// RUN: emitrust-import-c %s | FileCheck %s

// FR-92: the 2D-array-pointer cast shape — tiny-AES-c's remaining core.
// `typedef uint8_t state_t[4][4]` gives the Cipher family a
// pointer-to-array parameter, which maps to a whole-array reference
// (`!emitrust.mut_ref<!emitrust.array<4x!emitrust.array<4xui8>>>`,
// rendered `&mut [[u8; 4]; 4]`) whose body indexes `(*state)[i][j]` as
// deref + chained subscripts. This file pins the THREE shapes the FR
// admits on top of that mapping:
//   (1) FORWARDING — `Cipher(state, rk)` passes its own pointer-to-array
//       parameter through to the family (aes.c:413/439: Cipher/InvCipher
//       never index, they only forward). A call-argument appearance
//       historically classified the parameter as a Slice, which
//       rejected ("unsupported: slice parameter element type") because a
//       slice cannot view array elements; a pointer-to-constant-size-
//       array pointee now stays a scalar (whole-array) reference and the
//       block argument itself is the call operand (Rust's implicit
//       reborrow makes the bare forward legal).
//   (2) THE CAST — `Cipher((state_t*)buf, rk)` reinterprets a byte run
//       as the 4x4 state at the SAME u8 element (layout identity, no
//       transmute): the source reslices to `&mut [u8]` through the
//       existing slice-argument machinery, and the on-demand module-level
//       `__emitrust_chunk_4x4` helper reshapes it into `&mut [[u8;4];4]`.
//       All three aes.c source flavors are pinned: a plain `uint8_t*`
//       parameter (ECB, aes.c:473), a WALKING cursor mid-loop (CBC,
//       aes.c:508 `buf += 16`), and a local byte array (CTR, aes.c:550).
//   (3) the helper itself, emitted ONCE per module after all imported
//       items (the `__emitrust_cstr` precedent); a runtime under-length
//       view panics (the accepted refinement direction), a statically
//       undersized source rejects at import (array-2d-pointer-invalid.c).

typedef unsigned char uint8_t;
typedef uint8_t state_t[4][4];

// The deref-only family member: whole-array mut_ref param, runtime
// double subscript (the pre-FR-92 mapping, pinned as the differential
// base the new shapes compose with).
// CHECK-LABEL: func.func @SubOne(
// CHECK-SAME: %[[ST:.+]]: !emitrust.mut_ref<!emitrust.array<4x!emitrust.array<4xui8>>>
// CHECK: %[[P:.+]] = emitrust.deref %[[ST]] : (!emitrust.mut_ref<!emitrust.array<4x!emitrust.array<4xui8>>>) -> !emitrust.lvalue<!emitrust.array<4x!emitrust.array<4xui8>>>
// CHECK: %[[ROW:.+]] = emitrust.subscript %[[P]][{{.+}}] : (!emitrust.lvalue<!emitrust.array<4x!emitrust.array<4xui8>>>, {{.+}}) -> !emitrust.lvalue<!emitrust.array<4xui8>>
// CHECK: emitrust.subscript %[[ROW]][{{.+}}] : (!emitrust.lvalue<!emitrust.array<4xui8>>, {{.+}}) -> !emitrust.lvalue<ui8>
static void SubOne(state_t* state) {
  uint8_t i, j;
  for (i = 0; i < 4; ++i)
    for (j = 0; j < 4; ++j)
      (*state)[i][j] ^= (uint8_t)(i * 4u + j);
}

// Forwarding (Cipher/InvCipher): the parameter stays the whole-array
// reference and the call operand is the bare block argument.
// CHECK-LABEL: func.func @Cipher(
// CHECK-SAME: %[[CST:.+]]: !emitrust.mut_ref<!emitrust.array<4x!emitrust.array<4xui8>>>
// CHECK-SAME: %[[RK:.+]]: !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: call @SubOne(%[[CST]]) : (!emitrust.mut_ref<!emitrust.array<4x!emitrust.array<4xui8>>>) -> ()
// CHECK: call @SubOne(%[[CST]]) : (!emitrust.mut_ref<!emitrust.array<4x!emitrust.array<4xui8>>>) -> ()
static void Cipher(state_t* state, const uint8_t* rk) {
  uint8_t r;
  for (r = 0; r < rk[0]; ++r)
    SubOne(state);
  SubOne(state);
}

// Flavor A (ECB): the cast source is a plain `uint8_t*` parameter — a
// byte slice resliced at its cursor, then reshaped by the helper.
// CHECK-LABEL: func.func @ecb(
// CHECK-SAME: %[[BUF:.+]]: !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK: %[[EB:.+]] = emitrust.deref %[[BUF]]
// CHECK: %[[EW:.+]] = emitrust.slice_of mut %[[EB]][{{.+}}] : (!emitrust.lvalue<!emitrust.slice<ui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK: %[[EC:.+]] = emitrust.call_opaque "__emitrust_chunk_4x4"(%[[EW]]) : (!emitrust.mut_ref<!emitrust.slice<ui8>>) -> !emitrust.mut_ref<!emitrust.array<4x!emitrust.array<4xui8>>>
// CHECK: call @Cipher(%[[EC]], {{.+}}) : (!emitrust.mut_ref<!emitrust.array<4x!emitrust.array<4xui8>>>, !emitrust.ref<!emitrust.slice<ui8>>) -> ()
void ecb(uint8_t* buf, const uint8_t* rk) {
  Cipher((state_t*)buf, rk);
}

// Flavor B (CBC): the cast source WALKS (`buf += 16`), so the reslice
// happens at the runtime cursor — mid-loop the window starts at a
// nonzero offset.
// CHECK-LABEL: func.func @cbc(
// CHECK: %[[CC:.+]] = memref.load
// CHECK: %[[CW:.+]] = emitrust.slice_of mut %{{.+}}[%[[CC]]] : (!emitrust.lvalue<!emitrust.slice<ui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK: %[[CK:.+]] = emitrust.call_opaque "__emitrust_chunk_4x4"(%[[CW]])
// CHECK: call @Cipher(%[[CK]],
void cbc(uint8_t* buf, unsigned len, const uint8_t* rk) {
  unsigned off;
  for (off = 0; off < len; off += 16) {
    Cipher((state_t*)buf, rk);
    buf += 16;
  }
}

// Flavor C (CTR): the cast source is a LOCAL byte array — whole-array
// reslice at constant cursor 0 (16 bytes, exactly the 4x4 extent the
// import-time check requires).
// CHECK-LABEL: func.func @ctr(
// CHECK: %[[LB:.+]] = emitrust.variable named "buffer" : !emitrust.lvalue<!emitrust.array<16xui8>>
// CHECK: %[[LW:.+]] = emitrust.slice_of mut %[[LB]][{{.+}}] : (!emitrust.lvalue<!emitrust.array<16xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK: %[[LC:.+]] = emitrust.call_opaque "__emitrust_chunk_4x4"(%[[LW]])
// CHECK: call @Cipher(%[[LC]],
void ctr(const uint8_t* rk) {
  uint8_t buffer[16];
  Cipher((state_t*)buffer, rk);
}

// The reshaping helper: once per module, after all imported items, and
// exactly one instance for the one (4, 4) shape all three flavors share.
// CHECK: emitrust.verbatim "fn __emitrust_chunk_4x4(b: &mut [u8]) -> &mut {{\[\[}}u8; 4]; 4] {
// CHECK-SAME: as_chunks_mut::<4>()
// CHECK-SAME: try_into().unwrap()
// CHECK-NOT: emitrust.verbatim "fn __emitrust_chunk
