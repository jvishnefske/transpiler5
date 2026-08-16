// REQUIRES: cargo
// FR-96 differential end-to-end test: the two-record member-held FAM
// lifecycle — a FAM container (`struct enc`, u8 tail) holding a pointer to a
// second admitted FAM record (`struct hs_index`, i16 tail) as an OWNED
// NULLABLE member (`Option<hs_index>`), reduced from heatshrink_encoder.c:
//   - alloc-into-member:  hse->search_index = malloc(k*2 + sizeof(S))  (:92-98)
//     with the FAITHFUL (never elided) is_none failure guard
//   - member-of-member:   hse->search_index->size r/w                  (:98,:109)
//   - member-read local:  struct hs_index *hsi = hse->search_index,
//     then hsi->index[i] read/write in loops while the container's OWN
//     buffer tail is live in the same scope (:420-436, :463-487) — every
//     use is a fresh as_mut().unwrap() projection, never a bound borrow
//   - chain-walk reads:   pos = hsi->index[pos]  (find_longest_match)
//   - null tests, both polarities, on BOTH runtime paths (argc == 1 keeps
//     the member None; argc == 4 allocates)
//   - NULL-assignment and free-of-member -> None; the owned free wrapper
//     (enc_free) consumes the container BY VALUE and drops both records
// All sizes and fills are seeded from argc so no extent can fold at import
// time; the crate's stdout must byte-match the natively compiled C program
// for argc = 1 (None path) and argc = 4 (Some path) — a divergence in the
// member's payload extent, a projection aliasing error, or a dropped None
// store shifts a printed byte.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n1.out
// RUN: %t.crate/target/release/flexible_array_owned_member > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b c > %t.n4.out
// RUN: %t.crate/target/release/flexible_array_owned_member a b c > %t.r4.out
// RUN: diff %t.n4.out %t.r4.out

#include <stdio.h>
#include <stdlib.h>

struct hs_index {
  unsigned short size;
  short index[];
};

struct enc {
  unsigned short input_size;
  struct hs_index *search_index;
  unsigned char buffer[];
};

/* do_indexing's shape: member-read local, then projected element WRITES in a
 * loop that also reads AND writes the container's own tail. index[i] = i-1
 * builds the chain walk() follows (index[0] = -1 terminates it). */
static void do_index(struct enc *hse) {
  struct hs_index *hsi = hse->search_index;
  unsigned short i;
  for (i = 0; i < hse->input_size; i++) {
    hsi->index[i] = (short)((short)i - 1);
    hse->buffer[i] = (unsigned char)(hse->buffer[i] + i);
  }
}

/* find_longest_match's shape: chain-walk reads pos = hsi->index[pos] while
 * the container's buffer is read live in the same scope. */
static unsigned short walk(struct enc *hse, unsigned short start) {
  struct hs_index *hsi = hse->search_index;
  short pos = hsi->index[start];
  unsigned short acc = (unsigned short)hse->buffer[start];
  while (pos >= 0) {
    acc = (unsigned short)(acc * 31u + hse->buffer[(unsigned short)pos]);
    pos = hsi->index[pos];
  }
  return acc;
}

/* heatshrink_encoder_free's shape: member-of-member read, free-of-member,
 * then free of the container — the FR-94 owned free wrapper. */
static void enc_free(struct enc *hse) {
  size_t index_sz = sizeof(struct hs_index) + hse->search_index->size;
  free(hse->search_index);
  free(hse);
  (void)index_sz;
}

int main(int argc, char **argv) {
  size_t n = (size_t)argc + 4;
  struct enc *e = malloc(sizeof(struct enc) + n);
  if (e == NULL) {
    return 1;
  }
  e->input_size = (unsigned short)n;
  for (size_t i = 0; i < n; i++) {
    e->buffer[i] = (unsigned char)(i * 3 + (size_t)argc);
  }
  if (argc > 1) {
    size_t index_sz = n * sizeof(unsigned short);
    e->search_index = malloc(index_sz + sizeof(struct hs_index));
    if (e->search_index == NULL) {
      free(e);
      return 1;
    }
    e->search_index->size = (unsigned short)index_sz;
  } else {
    e->search_index = NULL;
  }
  if (e->search_index == NULL) {
    printf("no index (argc=%d)\n", argc);
  }
  if (e->search_index != NULL) {
    do_index(e);
    long sum = 0;
    struct hs_index *hsi = e->search_index;
    for (unsigned short i = 0; i < e->input_size; i++) {
      sum += hsi->index[i];
    }
    printf("sum=%ld size=%u walk=%u\n", sum,
           (unsigned)e->search_index->size,
           (unsigned)walk(e, (unsigned short)(n - 1)));
  }
  if (e->search_index != NULL) {
    enc_free(e);
  } else {
    free(e);
  }
  return 0;
}
