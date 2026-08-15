// REQUIRES: cargo
// FR-87: differential end-to-end test for MEMBER-ARRAY regions of the
// hosted byte family — memset/memcpy/memmove/memcmp whose region
// argument is a struct member array (dot, arrow, nested-u32, offset
// cursor, decomposed null-checked root — tinycrypt ctr_prng's
// uninstantiate shape). Mutation is VISIBLE after every call: the
// offset memset's bytes are read back by the following memcpy, the
// same-field memcpy/memmove ride `copy_within` (overlap-correct), and
// the u32 word fill must be the replicated byte 0xAB * 0x01010101 —
// the spike's byte-diff caught a hand-picked word constant here, so
// every filled word is printed. All seeds and the runtime offset
// derive from argc so constant folding cannot pre-compute the buffers
// and hide a miscompile; every buffer byte and word is printed.
// `cargo build` success alone proves nothing here — the stdout diff is
// the oracle. Deterministic, no UB (off = argc + 1 stays in bounds for
// both runs below).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/string_fn_member_region > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/string_fn_member_region a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>
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
/* A separate context type for the decomposed (null-checked) root so
   its pointer-parameter class stays single-base. */
struct KD {
  unsigned int words[11];
};
struct D {
  struct KD key;
  unsigned char V[16];
  unsigned int reseedCount;
};

static void scrub(struct C *c, unsigned int k) {
  memset(&c->V[k], 0xAB, 4);           /* arrow + argc-seeded offset */
  memcpy(c->W, c->V, sizeof c->W);     /* observes the memset above */
  memcpy(c->V, c->V + 8, 8);           /* same-field: copy_within */
  memmove(&c->W[k], c->W, 8);          /* overlapping memmove */
  memset(c->key.words, 0xAB, 8);       /* u32 partial fill: 2 words */
  /* Only memcmp's SIGN is specified by C (the magnitude is
     implementation-defined), so normalize before printing. */
  int r = memcmp(c->V, c->W, 16);
  c->n = r > 0 ? 1u : (r < 0 ? 2u : 0u);
}

/* tinycrypt tc_ctr_prng_uninstantiate, verbatim shape: decomposed
   null-checked root, u32 nested memset then byte memset. */
static void wipe(struct D *ctx) {
  if (0 != ctx) {
    memset(ctx->key.words, 0x00, sizeof ctx->key.words);
    memset(ctx->V, 0x00, sizeof ctx->V);
    ctx->reseedCount = 77u;
  }
}

int main(int argc, char **argv) {
  struct C c;
  struct D d[1];
  unsigned int i;
  unsigned int off = (unsigned int)argc + 1u;
  unsigned char seed = (unsigned char)(argc * 7);
  for (i = 0u; i < 16u; i++) {
    c.V[i] = (unsigned char)(seed + i);
    c.W[i] = (unsigned char)(seed * 3u + i);
    d[0].V[i] = (unsigned char)(seed + 100u + i);
  }
  for (i = 0u; i < 8u; i++)
    c.key.words[i] = 0x01020304u * ((unsigned int)seed + i);
  for (i = 0u; i < 11u; i++)
    d[0].key.words[i] = 0xDEADBEEFu + i * (unsigned int)argc;
  c.n = 0u;
  d[0].reseedCount = (unsigned int)argc;
  /* Dot-root region calls in main. */
  memset(c.W, 0x11, 4);
  memcpy(&c.W[4], &c.V[off], 4);       /* disjoint siblings, offsets */
  scrub(&c, off);
  printf("cmp %d\n", (int)c.n);
  printf("cmp2 %d\n", memcmp(c.V, c.V, 16)); /* same field, shared */
  for (i = 0u; i < 16u; i++)
    printf("V %u\n", (unsigned int)c.V[i]);
  for (i = 0u; i < 16u; i++)
    printf("W %u\n", (unsigned int)c.W[i]);
  for (i = 0u; i < 8u; i++)
    printf("K %u\n", c.key.words[i]);
  wipe(d);
  for (i = 0u; i < 11u; i++)
    printf("D %u\n", d[0].key.words[i]);
  for (i = 0u; i < 16u; i++)
    printf("DV %u\n", (unsigned int)d[0].V[i]);
  printf("rc %u\n", d[0].reseedCount);
  return 0;
}
