// REQUIRES: cargo
// FR-95 differential end-to-end test: a flexible-array-member record with a
// TYPED (int16_t) allocation-backed tail — the reduced heatshrink hs_index
// lifecycle (heatshrink_encoder.c's search index). All sizes are seeded from
// argc so nothing folds at import time:
//   - alloc: `size_t index_sz = n * sizeof(uint16_t);
//            malloc(index_sz + sizeof(struct hs_index))` (encoder.c:92-94) —
//     the count side routes through its own local, and the sizeof factor is
//     uint16_t over an int16_t tail (a NUMERIC fold, not a type match);
//   - the u16 `size` field stores the BYTE count (encoder.c:98) and must
//     stay byte-valued while the Vec extent is the ELEMENT count;
//   - chain-walk reads `pos = hsi->index[pos]` (encoder.c:474,487);
//   - the bare tail decay `int16_t *const index = hsi->index` with runtime
//     negative-value writes (encoder.c:425,433 — a byte tail could not hold
//     them);
//   - free of the record (encoder.c:109-110's wrapper shape).
// The record imports with an owned Vec<i16> tail (non-Copy struct), the
// alloc function returns the record BY VALUE, the free wrapper consumes it
// BY VALUE. The crate's stdout must byte-match the natively compiled C
// program for argc = 1 and argc = 4 — a byte-vs-element count confusion, a
// stride error in the decayed cursor, or a sign-lossy element type shifts a
// printed byte.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n1.out
// RUN: %t.crate/target/release/flexible_array_owned_typed_tail > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b c > %t.n4.out
// RUN: %t.crate/target/release/flexible_array_owned_typed_tail a b c > %t.r4.out
// RUN: diff %t.n4.out %t.r4.out

#include <stdio.h>
#include <stdlib.h>

struct hs_index {
  unsigned short size;
  short index[];
};

struct hs_index *idx_alloc(unsigned short n) {
  size_t index_sz = n * sizeof(unsigned short);
  struct hs_index *hsi = malloc(index_sz + sizeof(struct hs_index));
  if (hsi == NULL) return NULL;
  hsi->size = (unsigned short)index_sz; /* byte count, per the corpus */
  for (unsigned short i = 0; i < n; i++)
    hsi->index[i] = (short)(i - 1); /* slot i points at i-1; slot 0 -> -1 */
  return hsi;
}

void idx_free(struct hs_index *hsi) { free(hsi); }

int idx_walk(struct hs_index *hsi, short start) {
  int acc = 0;
  short pos = start;
  while (pos >= 0) {
    acc = acc * 31 + pos;
    pos = hsi->index[pos];
  }
  return acc;
}

int idx_rewrite(struct hs_index *hsi, unsigned short n) {
  short *const index = hsi->index; /* encoder.c:425's bare decay */
  int sum = 0;
  for (unsigned short i = 0; i < n; i++) {
    index[i] = (short)(1 - 3 * (short)(i % 7));
    sum += index[i];
  }
  return sum;
}

int main(int argc, char **argv) {
  unsigned short n = (unsigned short)(argc * 8 + 5);
  struct hs_index *hsi = idx_alloc(n);
  if (!hsi) return 1;
  int acc = idx_walk(hsi, hsi->index[n - 1]);
  int sum = idx_rewrite(hsi, n);
  printf("n=%u size=%u acc=%d sum=%d last=%d\n", (unsigned)n,
         (unsigned)hsi->size, acc, sum, (int)hsi->index[n - 1]);
  idx_free(hsi);
  return 0;
}
