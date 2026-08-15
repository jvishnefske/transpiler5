// REQUIRES: cargo
// FR-90: differential end-to-end test for member-ADDRESS arguments
// (`&s->field`, field STRUCT-typed) through a NULL-COMPARED — and
// therefore DECOMPOSED — struct-pointer parameter, the exact residual
// behind tinycrypt's `tc_aes_encrypt(..., &ctx->key)` and
// `tc_hmac_init(&prng->h)` sites. BOTH decomposed spellings run:
// `update` is driven with TWO distinct local struct arrays, which
// forces the Phase-1b SLICE decomposition (the corpus's --incremental
// shape), while `update_solo` is driven with ONE array and
// OWNER-promotes — same C, the other lowering. Each exercises the
// straight-line mutable borrow, the SAME borrow inside a while loop
// (the hmac_prng.c:210 shape — mutation through the borrowed member
// must compound across iterations), the disjoint-sibling double borrow
// (`two(&s->h, &s->g)` — writes through BOTH borrows in one call), the
// MIXED addr_of + shared member-array slice_of pair over one root
// (`mixed(&s->h, s->key, ...)` — the hmac_prng generate() shape), and a
// shared borrow feeding a fold. All seeds derive from argc so constant
// folding cannot hide a miscompile; every field of every struct is
// printed and byte-diffed against the clang-built native. `cargo build`
// success alone proves nothing here — the stdout diff is the oracle.
// Deterministic, no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/member_struct_addr_arg > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/member_struct_addr_arg a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>

struct Inner {
  unsigned int a;
  unsigned int b;
};
struct C {
  struct Inner h;
  struct Inner g;
  unsigned char key[8];
  unsigned int n;
};
typedef struct C *Cp;

/* No null check of its own: keeps the mut_ref receiver convention
   (tinycrypt's callees are decl-only across TUs; same convention). */
static void bump(struct Inner *x, unsigned int d) {
  x->a += d;
  x->b ^= x->a + 0x9du;
}

static unsigned int fold(const struct Inner *x) {
  return x->a * 3u + x->b;
}

/* Writes through BOTH disjoint-sibling borrows in one call. */
static void two(struct Inner *x, struct Inner *y) {
  x->a += y->b;
  y->b ^= x->a;
}

/* Mixed pair: mutable struct borrow + shared byte slice of a sibling
   member array of the SAME root. */
static void mixed(struct Inner *x, const unsigned char *k,
                  unsigned int len) {
  unsigned int i;
  for (i = 0u; i < len; i++)
    x->b += (unsigned int)k[i] * (i + 1u);
}

static void update(Cp s, unsigned int seed) {
  unsigned int n;
  if (s == (Cp)0)
    return;
  bump(&s->h, seed);
  n = seed % 5u + 2u;
  while (n != 0u) {
    bump(&s->h, n);
    bump(&s->g, s->h.a & 7u);
    n = n - 1u;
  }
  two(&s->h, &s->g);
  mixed(&s->h, s->key, 8u);
  s->n += fold(&s->h) & 15u;
}

static void update_solo(Cp s, unsigned int seed) {
  unsigned int n;
  if (s == (Cp)0)
    return;
  bump(&s->h, seed);
  n = seed % 5u + 2u;
  while (n != 0u) {
    bump(&s->h, n);
    bump(&s->g, s->h.a & 7u);
    n = n - 1u;
  }
  two(&s->h, &s->g);
  mixed(&s->h, s->key, 8u);
  s->n += fold(&s->h) & 15u;
}

/* seed/show are shared by the TWO-array pair (a, b) only — `solo` is
   seeded and printed inline so `update_solo` stays its ONLY callee and
   the owner promotion is preserved. */
static void seedC(struct C *c, unsigned int base) {
  unsigned int i;
  c->h.a = base + 1u;
  c->h.b = base * 5u + 2u;
  c->g.a = base * 3u + 7u;
  c->g.b = base ^ 0x2bu;
  for (i = 0u; i < 8u; i++)
    c->key[i] = (unsigned char)(base * 11u + i * 3u);
  c->n = 0u;
}

static void show(const struct C *c) {
  unsigned int i;
  printf("%u %u %u %u %u\n", c->h.a, c->h.b, c->g.a, c->g.b, c->n);
  for (i = 0u; i < 8u; i++)
    printf("%u\n", (unsigned int)c->key[i]);
}

int main(int argc, char **argv) {
  struct C a[1];
  struct C b[1];
  struct C solo[1];
  unsigned int i;
  unsigned int seed = (unsigned int)argc * 7u + 2u;
  seedC(a, seed);
  seedC(b, seed * 5u + 3u);
  solo[0].h.a = seed + 4u;
  solo[0].h.b = seed * 9u + 1u;
  solo[0].g.a = seed * 2u + 5u;
  solo[0].g.b = seed ^ 0x51u;
  for (i = 0u; i < 8u; i++)
    solo[0].key[i] = (unsigned char)(seed * 13u + i * 5u);
  solo[0].n = 0u;
  update(a, seed);
  update(a, seed + 3u);
  update(b, seed + 1u);
  update(b, seed + 6u);
  update_solo(solo, seed + 2u);
  update_solo(solo, seed + 5u);
  show(a);
  show(b);
  printf("%u %u %u %u %u\n", solo[0].h.a, solo[0].h.b, solo[0].g.a,
         solo[0].g.b, solo[0].n);
  for (i = 0u; i < 8u; i++)
    printf("%u\n", (unsigned int)solo[0].key[i]);
  return 0;
}
