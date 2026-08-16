// REQUIRES: cargo
// FR-97: differential end-to-end test for byte-splat memset over TYPED
// integer arrays — LOCAL (i16/u16/i32/u32/i64/u64) and MEMBER (i16/u16)
// destinations, fills 0x00 / 0xFF / 0xAB, counts spelled sizeof() and
// as partial constants. The fill WORD must be the per-width replicated
// byte (b * 0x0101 / 0x01010101 / 0x0101010101010101): 0xAB is the
// non-symmetric proof byte — a hand-picked replication constant is
// exactly the miscompile FR-87's byte-diff caught — and the 0xFF-u64
// arm enforces the exact all-ones bit pattern (u64::MAX). Every array
// is seeded from argc BEFORE the splats and read back element by
// element through TYPED indexing after them (partial fills leave
// argc-seeded survivors visible), so constant folding cannot
// pre-compute the buffers and hide a miscompile; an argc-picked index
// is written and read back after the fill. `cargo build` success alone
// proves nothing here — the stdout diff against the clang-built native
// is the oracle. Deterministic, no UB (argc % 8 stays in bounds for
// both runs below).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/memset_typed_splat > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/memset_typed_splat a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>
#include <string.h>
#include <stdint.h>

struct S {
  int16_t last[8];
  uint16_t w[4];
  unsigned int n;
};

/* Arrow-root MEMBER destinations: the do_indexing shape (0xFF over
   i16) plus the u16 sibling with the 0xAB proof byte. */
static void scrub(struct S *s) {
  memset(s->last, 0xFF, sizeof(s->last));
  memset(s->w, 0xAB, sizeof(s->w));
  s->n += 1u;
}

int main(int argc, char **argv) {
  int16_t a[8];
  uint16_t b[4];
  int32_t q[4];
  unsigned int lw[4];
  int64_t r[3];
  uint64_t u[2];
  struct S s;
  int i;
  int16_t seed = (int16_t)(argc * 3);
  for (i = 0; i < 8; i++) {
    a[i] = (int16_t)(seed + i);
    s.last[i] = (int16_t)(seed - i);
  }
  for (i = 0; i < 4; i++) {
    b[i] = (uint16_t)(argc * 9 + i);
    q[i] = argc + i;
    lw[i] = (unsigned int)argc + (unsigned int)i;
  }
  for (i = 0; i < 4; i++)
    s.w[i] = (uint16_t)(argc * 5 + i);
  for (i = 0; i < 3; i++)
    r[i] = (int64_t)argc * 1000 + i;
  for (i = 0; i < 2; i++)
    u[i] = (uint64_t)argc * 7u + (uint64_t)i;
  s.n = (unsigned int)argc;

  memset(a, 0x00, sizeof(a));
  for (i = 0; i < 8; i++)
    printf("z %d\n", (int)a[i]);
  memset(a, 0xFF, sizeof(a));
  for (i = 0; i < 8; i++)
    printf("f %d\n", (int)a[i]);
  memset(a, 0xAB, sizeof(a));
  a[argc % 8] = seed; /* typed write-back after the splat */
  for (i = 0; i < 8; i++)
    printf("m %d\n", (int)a[i]);
  memset(b, 0xFF, 4); /* u16 PARTIAL: 2 elements, 2 seeded survivors */
  for (i = 0; i < 4; i++)
    printf("b %u\n", (unsigned int)b[i]);
  memset(q, 0xAB, sizeof(q));
  for (i = 0; i < 4; i++)
    printf("q %d\n", q[i]);
  memset(lw, 0xAB, 8); /* local u32 PARTIAL: 2 words, 2 survivors */
  for (i = 0; i < 4; i++)
    printf("lw %u\n", lw[i]);
  memset(r, 0xAB, sizeof(r));
  for (i = 0; i < 3; i++)
    printf("r %lld\n", (long long)r[i]);
  memset(u, 0xFF, sizeof(u));
  for (i = 0; i < 2; i++)
    printf("u %llu\n", (unsigned long long)u[i]);

  scrub(&s);
  for (i = 0; i < 8; i++)
    printf("s %d\n", (int)s.last[i]);
  for (i = 0; i < 4; i++)
    printf("w %u\n", (unsigned int)s.w[i]);
  printf("pick %d %d %u\n", (int)a[argc % 8], (int)s.last[argc % 8], s.n);
  return 0;
}
