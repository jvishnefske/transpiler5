// REQUIRES: cargo
// FR-91: differential end-to-end test for member-array windows on a
// BYTE-REGION root — the tiny-AES-c shapes that motivated the FR
// (aes.c:221 `KeyExpansion(ctx->RoundKey, key)` window as a slice
// ARGUMENT, aes.c:231 `memcpy(ctx->Iv, iv, n)` window as the memcpy
// DESTINATION, aes.c:549 `memcpy(buffer, ctx->Iv, n)` window as the
// SHARED SOURCE inside a ctr loop that also steps the Iv counter
// through region writes). The all-u8 struct imports as one flat byte
// region; each member argument is an OPEN-ENDED `slice_of` window at
// the member's layout offset plus the parameter's runtime cursor, so
// an off-by-one window start rewrites/reads the WRONG region bytes
// and the byte diff catches it. The same-field memmove runs with
// RUNTIME offsets in BOTH overlap directions (dst below src and dst
// above src) — copy_within's memmove semantics against the native
// memmove — and the same-root cross-field memcpy rides the same
// whole-region image with absolute cursors. All stored values derive
// from argc so constant folding cannot pre-compute a buffer and hide
// a miscompile; the second RUN pair re-seeds via extra argv words.
// `cargo build` success alone proves nothing here — the stdout diff
// against the clang-built native is the oracle. Deterministic, no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/byte_region_member_window > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/byte_region_member_window a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>
#include <string.h>

typedef unsigned char u8;
struct Ctx { u8 RoundKey[176]; u8 Iv[16]; };

/* The defined callee taking the window as a mut slice parameter. */
static void key_expansion(u8 *rk, const u8 *key) {
  unsigned i;
  for (i = 0u; i < 176u; i++)
    rk[i] = (u8)(key[i % 16u] + (u8)(i * 7u));
}

static void init_ctx(struct Ctx *ctx, const u8 *key, const u8 *iv) {
  key_expansion(ctx->RoundKey, key); /* window as slice ARG (mut) */
  memcpy(ctx->Iv, iv, 16);           /* window as memcpy DST */
}

/* Iv counter step through region element writes (aes.c ctr shape). */
static void ctr_step(struct Ctx *ctx) {
  int i;
  for (i = 15; i >= 0; i--) {
    if (ctx->Iv[i] == 255u) {
      ctx->Iv[i] = 0u;
      continue;
    }
    ctx->Iv[i] = (u8)(ctx->Iv[i] + 1u);
    break;
  }
}

static void xcrypt(struct Ctx *ctx, u8 *buf, unsigned len) {
  u8 stream[16];
  unsigned i;
  for (i = 0u; i < len; i++) {
    if (i % 16u == 0u) {
      memcpy(stream, ctx->Iv, 16); /* window as SHARED memcpy SRC */
      ctr_step(ctx);
    }
    buf[i] = (u8)(buf[i] ^ (u8)(stream[i % 16u] + ctx->RoundKey[i % 176u]));
  }
}

/* Same-field memmove, runtime offsets, dst BELOW src... */
static void shift_down(struct Ctx *ctx, unsigned k, unsigned n) {
  memmove(&ctx->Iv[0], &ctx->Iv[k], n);
}

/* ...and dst ABOVE src: both overlap directions hit copy_within. */
static void shift_up(struct Ctx *ctx, unsigned k, unsigned n) {
  memmove(&ctx->Iv[k], &ctx->Iv[0], n);
}

/* Same-root cross-field memcpy: absolute region cursors 176 and 0. */
static void mix(struct Ctx *ctx) {
  memcpy(ctx->Iv, ctx->RoundKey, 16);
}

static void dump(const char *tag, const u8 *p, unsigned n) {
  unsigned i;
  printf("%s:", tag);
  for (i = 0u; i < n; i++)
    printf(" %02x", (unsigned)p[i]);
  printf("\n");
}

int main(int argc, char **argv) {
  struct Ctx ctx;
  u8 key[16];
  u8 iv[16];
  u8 buf[40];
  unsigned i;
  unsigned k = (unsigned)argc % 4u + 1u;
  for (i = 0u; i < 16u; i++) {
    key[i] = (u8)(argc * 13 + (int)i * 5 + 7);
    iv[i] = (u8)(argc * 29 + (int)i * 3 + 250);
  }
  for (i = 0u; i < 40u; i++)
    buf[i] = (u8)(argc * 11 + (int)i);
  init_ctx(&ctx, key, iv);
  dump("rk", ctx.RoundKey, 176u);
  dump("iv", ctx.Iv, 16u);
  xcrypt(&ctx, buf, 40u);
  dump("buf", buf, 40u);
  dump("iv2", ctx.Iv, 16u);
  shift_down(&ctx, k, 8u);
  dump("dn", ctx.Iv, 16u);
  shift_up(&ctx, k, 8u);
  dump("up", ctx.Iv, 16u);
  mix(&ctx);
  dump("mix", ctx.Iv, 16u);
  return 0;
}
