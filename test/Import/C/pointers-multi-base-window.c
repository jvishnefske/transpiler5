// RUN: emitrust-import-c %s | FileCheck %s

// FR-93, the aes cbc loop shape (aes.c AES_CBC_encrypt_buffer): a
// pointer local initialized from a byte-region member WINDOW
// (`uint8_t *Iv = ctx->Iv;`) and rebound to a walking slice parameter
// inside the loop (`Iv = buf;`). The region has TWO cursored bases —
// the ctx byte region (window origin = the field's byte offset) and
// the buf parameter — so the local carries the CTS-P7 enum-of-bases
// discriminant plus ONE absolute i64 cursor, interpreted in the
// active base's coordinates. Three seams are pinned here:
//   (1) passing the multi-base local to a defined callee dispatches
//       the WHOLE CALL on the discriminant (plain cf CFG), each arm
//       materializing its base's open-ended slice view at the cursor;
//   (2) the arm whose base COLLIDES with another mutable slice
//       argument of the same call (`xw(buf, Iv)` once Iv == buf's
//       region) splits the base with `__emitrust_split_mut_u8`
//       (split_at_mut at the mutable cursor; the const window reads
//       strictly below it, out-of-window reads panic — the accepted
//       loud refinement of C reads) instead of rejecting the aliasing
//       pair;
//   (3) the tail `memcpy(ctx->iv, Iv, 4)` with the multi-base SOURCE
//       dispatches per base: the same-root window arm rides the
//       whole-region `__emitrust_memcpy_within_u8` image (absolute
//       cursors; memmove semantics refine C's undefined exact-overlap
//       self-copy) and the cross-root arm the two-slice
//       `__emitrust_memcpy_u8` image.
// `cargo build` cannot see a wrong arm — the EndToEnd byte-diff twin
// (byte-region-cbc-walk.c) is the correctness oracle; this golden pins
// the IR shape so the dispatch structure cannot silently degrade.

#include <string.h>

typedef unsigned char u8;
typedef unsigned long size_t_;
struct C { u8 rk[8]; u8 iv[4]; };

static void xw(u8 *buf, const u8 *iv) {
  unsigned i;
  for (i = 0u; i < 4u; i++)
    buf[i] ^= iv[i];
}

static void enc(struct C *ctx, u8 *buf, size_t_ len) {
  size_t_ i;
  u8 *Iv = ctx->iv;
  for (i = 0; i < len; i += 4) {
    xw(buf, Iv);
    Iv = buf;
    buf += 4;
  }
  memcpy(ctx->iv, Iv, 4);
}
// CHECK-LABEL: func.func @enc
//   the discriminant cell and the absolute i64 cursor cells.
// CHECK-DAG: memref.alloca() : memref<i32>
// CHECK-DAG: memref.alloca() : memref<i64>
//   the init binding selects base 0 (the ctx window) at byte offset 8.
// CHECK: arith.constant 8 : i64
//   (1) the xw call's discriminant test, then (blocks print in creation
//   order) the tail memcpy's discriminant test...
// CHECK: arith.cmpi eq
// CHECK: cf.cond_br
// CHECK: arith.cmpi eq
// CHECK: cf.cond_br
//   ...then xw arm 0: the ctx region view at the shared cursor next to
//   buf's own view, one call per arm...
// CHECK: emitrust.slice_of %{{.*}} : (!emitrust.lvalue<!emitrust.slice<ui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: emitrust.slice_of mut
// CHECK: call @xw
//   ...and the same-base arm: split buf at its mutable cursor, bound
//   the const window strictly below it.
// CHECK: emitrust.call_opaque "__emitrust_split_mut_u8"(%{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, i64) -> (!emitrust.mut_ref<!emitrust.slice<ui8>>, !emitrust.mut_ref<!emitrust.slice<ui8>>)
// CHECK: emitrust.deref
// CHECK: emitrust.slice_of
// CHECK: call @xw
//   the rebind `Iv = buf` stores discriminant 1 + buf's cursor.
// CHECK: arith.constant 1 : i32
// CHECK: memref.store
//   (3) the tail memcpy arms: window arm = whole-region copy_within
//   image with absolute cursors, cross-root arm = two-slice image.
// CHECK: emitrust.call_opaque "__emitrust_memcpy_within_u8"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, i64, i64, i64) -> ()
// CHECK: emitrust.call_opaque "__emitrust_memcpy_u8"(%{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, !emitrust.ref<!emitrust.slice<ui8>>, i64) -> ()

int main(void) {
  struct C ctx;
  u8 data[16];
  unsigned i;
  for (i = 0u; i < 8u; i++)
    ctx.rk[i] = (u8)i;
  for (i = 0u; i < 4u; i++)
    ctx.iv[i] = (u8)(3u * i + 1u);
  for (i = 0u; i < 16u; i++)
    data[i] = (u8)(i * 5u);
  enc(&ctx, data, 16);
  return (int)ctx.iv[0];
}
