// REQUIRES: cargo
// FR-93, differential end-to-end test: the REAL AES_CBC_encrypt_buffer
// walk shape (aes.c:504's `uint8_t *Iv = ctx->Iv;`, tiny-AES-c's last
// blocker). Iv is a MULTI-BASE cursor local — base 0 the ctx byte
// region (window origin = Iv's layout offset 176), base 1 the walking
// buf parameter — and the loop exercises every Phase-2 seam at once:
// the whole-call discriminant dispatch of `XorWithIv(buf, Iv)`, whose
// same-base arm (Iv == buf's region after the first iteration, with
// iv_cur = buf_cur - 16 strictly below the mutable cursor) must ride
// `split_at_mut`; the `Cipher(buf, ctx->RoundKey)` window argument in
// the SAME loop (the FR-91 form, proving the borrows compose per-use);
// the rebind `Iv = buf` + `buf += 16`; and the tail `memcpy(ctx->Iv,
// Iv, 16)` dispatching between the same-root `copy_within` image and
// the cross-root two-slice image. The length==0 call keeps Iv
// window-backed so the EXACT-OVERLAP self-copy arm runs (memmove
// semantics refine C's undefined exact-overlap memcpy — byte-identical
// result). Every byte derives from argc so constant folding cannot
// hide a wrong arm, an off-by-16 window origin, or a shifted split
// boundary; `cargo build` success alone proves nothing here — the
// stdout diff against the clang-built native is the oracle.
// Deterministic, no UB; main returns 0.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/byte_region_cbc_walk > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/byte_region_cbc_walk a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>
#include <string.h>

typedef unsigned char uint8_t;
struct AES_ctx { uint8_t RoundKey[176]; uint8_t Iv[16]; };

static void XorWithIv(uint8_t *buf, const uint8_t *Iv) {
  uint8_t i;
  for (i = 0; i < 16; ++i)
    buf[i] ^= Iv[i];
}

static void Cipher(uint8_t *state, const uint8_t *RoundKey) {
  int i;
  for (i = 0; i < 16; ++i)
    state[i] = (uint8_t)(state[i] + RoundKey[(i * 7 + 3) % 176] + 1);
}

static void AES_CBC_encrypt_buffer(struct AES_ctx *ctx, uint8_t *buf,
                                   unsigned long length) {
  unsigned long i;
  uint8_t *Iv = ctx->Iv;
  for (i = 0; i < length; i += 16) {
    XorWithIv(buf, Iv);
    Cipher(buf, ctx->RoundKey);
    Iv = buf;
    buf += 16;
  }
  /* store Iv in ctx for next call */
  memcpy(ctx->Iv, Iv, 16);
}

int main(int argc, char **argv) {
  struct AES_ctx ctx;
  uint8_t buf[64];
  unsigned i;
  for (i = 0; i < 176; ++i)
    ctx.RoundKey[i] = (uint8_t)(i * 3 + (unsigned)argc);
  for (i = 0; i < 16; ++i)
    ctx.Iv[i] = (uint8_t)(i + 17u * (unsigned)argc);
  for (i = 0; i < 64; ++i)
    buf[i] = (uint8_t)(i ^ ((unsigned)argc * 5u));
  AES_CBC_encrypt_buffer(&ctx, buf, 64);
  for (i = 0; i < 64; ++i)
    printf("%02x", (unsigned)buf[i]);
  printf("\n");
  for (i = 0; i < 16; ++i)
    printf("%02x", (unsigned)ctx.Iv[i]);
  printf("\n");
  /* length 0: Iv never rebinds — the window-backed exact-overlap
     self-copy arm of the tail memcpy (the real aes shape when the
     buffer is empty). */
  AES_CBC_encrypt_buffer(&ctx, buf, 0);
  for (i = 0; i < 16; ++i)
    printf("%02x", (unsigned)ctx.Iv[i]);
  printf("\n");
  return 0;
}
