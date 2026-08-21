// REQUIRES: cargo
// FR-99 differential end-to-end test: a FAM-record allocator with a REACHABLE
// `return NULL` lifts to an Option-typed owned return, and its two admitted
// call-site shapes must reproduce the C program byte for byte.
//   - `bag_alloc` / `enc_alloc` reject their argument with a leading
//     parameter validation (`n == 0 || n > 64`, heatshrink_encoder_alloc's
//     shape) -> `None`; the malloc-failure guard is still elided; the owned
//     return wraps `Some`.
//   - `enc_alloc` additionally allocates an FR-96 `Option<hs_index>` MEMBER
//     and returns `None` out of its FAITHFUL failure guard after `free(e)` —
//     the third NULL return of the real heatshrink_encoder_alloc, where the
//     free is the no-op drop of an owned local.
//   - The GUARDED bind (`e = enc_alloc(n); if (e == NULL) ...`, both the
//     `== NULL` and the `!p` polarity) must fold to the Option discriminant
//     and must NOT be elided: an elided guard would run the rejected-argument
//     legs through the payload path and print different bytes.
//   - The UNGUARDED bind unwraps at the binding; it is only ever reached with
//     an accepted argument here.
// Every extent and fill is seeded from argc so no size folds at import time,
// and both a REJECTED (`edge` = 120 at argc == 4) and an ACCEPTED (`edge` =
// 30 at argc == 1) argument reach the same call site across the two runs: a
// deleted guard, a swapped Some/None arm, or a lost member payload shifts a
// printed byte.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n1.out
// RUN: %t.crate/target/release/flexible_array_nullable_return > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b c > %t.n4.out
// RUN: %t.crate/target/release/flexible_array_nullable_return a b c > %t.r4.out
// RUN: diff %t.n4.out %t.r4.out

#include <stdio.h>
#include <stdlib.h>

typedef struct {
  unsigned short n;
  unsigned char buf[];
} bag;

struct hs_index {
  unsigned short size;
  short index[];
};

typedef struct {
  unsigned short n;
  struct hs_index *si;
  unsigned char buf[];
} enc;

/* The plain FR-99 shape: a parameter validation NULL, the elided malloc
   guard, and the owned return. */
static bag *bag_alloc(unsigned short n) {
  unsigned short i;
  if (n == 0 || n > 64) {
    return NULL;
  }
  bag *b = malloc(sizeof(bag) + n);
  if (b == NULL) {
    return NULL;
  }
  b->n = n;
  for (i = 0; i < n; i++) {
    b->buf[i] = (unsigned char)(i + n);
  }
  return b;
}

/* The heatshrink_encoder_alloc shape: THREE NULL returns — the validation,
   the elided malloc guard, and the member-allocation failure arm that frees
   the container first. */
static enc *enc_alloc(unsigned short n) {
  unsigned short i;
  if (n == 0 || n > 64) {
    return NULL;
  }
  enc *e = malloc(sizeof(enc) + n);
  if (e == NULL) {
    return NULL;
  }
  e->n = n;
  for (i = 0; i < n; i++) {
    e->buf[i] = (unsigned char)(i + n);
  }
  e->si = malloc(sizeof(struct hs_index) + n * sizeof(short));
  if (e->si == NULL) {
    free(e);
    return NULL;
  }
  e->si->size = n;
  return e;
}

/* GUARDED bind, `== NULL` polarity; the guard body exits. */
static unsigned bag_tally(unsigned short n) {
  unsigned short i;
  unsigned s;
  bag *b = bag_alloc(n);
  if (b == NULL) {
    return 0;
  }
  s = b->n;
  for (i = 0; i < b->n; i++) {
    s += b->buf[i];
  }
  free(b);
  return s;
}

/* GUARDED bind, `!p` polarity. */
static unsigned enc_tally(unsigned short n) {
  unsigned short i;
  unsigned s;
  enc *e = enc_alloc(n);
  if (!e) {
    return 111u;
  }
  s = e->si->size;
  for (i = 0; i < e->n; i++) {
    s += e->buf[i];
  }
  free(e->si);
  free(e);
  return s;
}

int main(int argc, char **argv) {
  unsigned short good;
  unsigned short edge;
  good = (unsigned short)(argc * 3);
  edge = (unsigned short)(argc * 30);
  printf("bag zero=%u big=%u good=%u edge=%u\n", bag_tally(0), bag_tally(999),
         bag_tally(good), bag_tally(edge));
  printf("enc zero=%u big=%u good=%u edge=%u\n", enc_tally(0), enc_tally(999),
         enc_tally(good), enc_tally(edge));
  /* UNGUARDED bind, always an accepted argument. */
  bag *u = bag_alloc(good);
  printf("u n=%u b0=%u last=%u\n", u->n, u->buf[0], u->buf[u->n - 1]);
  free(u);
  return 0;
}
