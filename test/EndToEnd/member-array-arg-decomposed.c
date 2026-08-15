// REQUIRES: cargo
// FR-86 (mechanism A): differential end-to-end test for member-array
// arguments through a NULL-COMPARED struct-pointer parameter — the
// decomposed representation that dominated the corpus's remaining
// ArrayToPointerDecay stubs (sha256's `compress(s->iv, s->leftover)`,
// ctr_prng's `arrInc(ctx->V, ...)`). BOTH decomposed spellings run:
// `update` is driven with TWO distinct local struct arrays, which
// forces the Phase-1b SLICE decomposition (the corpus's --incremental
// shape), while `update_solo` is driven with ONE array and
// OWNER-promotes — same C, the other lowering. Each passes two sibling
// member arrays of its decomposed root in ONE call (mutable u32 slice
// + shared byte slice, the disjoint-borrow shape), then a mutable
// single-member borrow into `arrInc`, whose u8 carry-wrap is exercised
// on purpose: leftover bytes are seeded 250..257 so increments wrap
// through 0 and a lowering that drops or double-applies a write shows
// up in the printed bytes. Updates run TWICE per state so carried
// struct state compounds. All seeds derive from argc; every iv word,
// every leftover byte, and the counters are printed and byte-diffed
// against the clang-built native. `cargo build` success alone proves
// nothing here — the stdout diff is the oracle. Deterministic, no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/member_array_arg_decomposed > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/member_array_arg_decomposed a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>

struct C {
  unsigned int iv[4];
  unsigned char leftover[8];
  unsigned int n;
};
typedef struct C *Cp;

static void compress_(unsigned int *iv, const unsigned char *data) {
  unsigned int i;
  for (i = 0u; i < 4u; i++)
    iv[i] += (unsigned int)data[i] * (i + 1u);
}

static void arrInc(unsigned char *arr, unsigned int len) {
  unsigned int i;
  if (0 != arr) {
    for (i = len; i > 0u; i--) {
      if (++arr[i - 1u] != 0u)
        break;
    }
  }
}

static void update(Cp s) {
  if (s == (Cp)0)
    return;
  compress_(s->iv, s->leftover);
  arrInc(s->leftover, 8u);
  s->n++;
}

static void update_solo(Cp s) {
  if (s == (Cp)0)
    return;
  compress_(s->iv, s->leftover);
  arrInc(s->leftover, 8u);
  s->n++;
}

/* seed/show are shared by the TWO-array pair (a, b) only — `solo` is
   seeded and printed inline so `update_solo` stays its ONLY callee and
   the owner promotion is preserved. */
static void seed(struct C *c, unsigned int base) {
  unsigned int i;
  for (i = 0u; i < 4u; i++)
    c->iv[i] = i * 3u + base;
  for (i = 0u; i < 8u; i++)
    c->leftover[i] = (unsigned char)(250u + i + base);
  c->n = 0u;
}

static void show(const struct C *c) {
  unsigned int i;
  for (i = 0u; i < 4u; i++)
    printf("%u\n", c->iv[i]);
  for (i = 0u; i < 8u; i++)
    printf("%u\n", (unsigned int)c->leftover[i]);
  printf("%u\n", c->n);
}

int main(int argc, char **argv) {
  struct C a[1];
  struct C b[1];
  struct C solo[1];
  unsigned int i;
  seed(a, (unsigned int)argc);
  seed(b, (unsigned int)argc * 5u);
  for (i = 0u; i < 4u; i++)
    solo[0].iv[i] = i * 3u + (unsigned int)argc + 2u;
  for (i = 0u; i < 8u; i++)
    solo[0].leftover[i] = (unsigned char)(252u + i + (unsigned int)argc);
  solo[0].n = 0u;
  update(a);
  update(a);
  update(b);
  update(b);
  update_solo(solo);
  update_solo(solo);
  show(a);
  show(b);
  for (i = 0u; i < 4u; i++)
    printf("%u\n", solo[0].iv[i]);
  for (i = 0u; i < 8u; i++)
    printf("%u\n", (unsigned int)solo[0].leftover[i]);
  printf("%u\n", solo[0].n);
  return 0;
}
