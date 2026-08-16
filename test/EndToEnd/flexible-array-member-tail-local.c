// REQUIRES: cargo
// FR-98 differential end-to-end test: FAM-tail pointer locals through
// Option-member projections, both composition arms byte-diffed against the
// clang-built native.
//   Arm A (member-read-local root, do_indexing's core):
//     index = hsi->index  where hsi is the FR-96 member-read local over the
//     lifted Option<hs_index> member — every use is a fresh
//     as_mut().unwrap() projection chain plus the tail subscript. The loop
//     interleaves the tail Vec WRITE through `index`, the buffer window
//     READ through `data` (the FR-95 param-root tail local), and a local
//     last[] read/write; chain_walk then READS through the same binding
//     (pos = index[pos]) while the container root stays live.
//   Arm B (element-run struct-pointer root):
//     ring = w->ring  where `w`'s place is an ELEMENT RUN of its struct —
//     here the FR-30 promoted owner receiver's data array (main's `ws`
//     array claims the whole call chain), the same root-cursor discipline
//     as the slice-classified corpus shape — so every use of the local
//     first subscripts the run at the ROOT'S OWN cursor. Two distinct
//     roots (cursor 0 and argc&1) drive one function — a stale or
//     mis-rooted cursor shifts a printed byte.
//   (The two arms live in separate call chains: an owned FAM local cannot
//   pass as a slice argument — the pre-existing FR-94 call-boundary wall —
//   so the FAM-container SLICE-root emission of Arm B is pinned at the IR
//   level by the Import golden; its byte-level behavior was proven by the
//   FR-98 spike hand-crate.)
// All sizes and fills are seeded from argc so no extent can fold at import
// time; the crate's stdout must byte-match the native for argc = 1 and 4.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n1.out
// RUN: %t.crate/target/release/flexible_array_member_tail_local > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b c > %t.n4.out
// RUN: %t.crate/target/release/flexible_array_member_tail_local a b c > %t.r4.out
// RUN: diff %t.n4.out %t.r4.out

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct hs_index {
  uint16_t size;
  int16_t index[];
};

struct enc {
  uint16_t input_size;
  struct hs_index *search_index;
  uint8_t buffer[];
};

/* Arm A: the do_indexing shape — tail WRITES through the member-read-root
 * local, window reads through the param-root local, last[] interleaved. */
static void do_indexing(struct enc *hse) {
  struct hs_index *hsi = hse->search_index;
  int16_t last[256];
  memset(last, 0xFF, sizeof(last));
  uint8_t *const data = hse->buffer;
  int16_t *const index = hsi->index;
  const uint16_t end = hse->input_size;
  for (uint16_t i = 0; i < end; i++) {
    uint8_t v = data[i];
    int16_t lv = last[v];
    index[i] = lv;
    last[v] = (int16_t)i;
  }
}

/* Arm A reads: the find_longest_match chain walk through the same binding,
 * with the container's own buffer live in the same scope. */
static int16_t chain_walk(struct enc *hse, uint16_t start) {
  struct hs_index *hsi = hse->search_index;
  int16_t *const index = hsi->index;
  int16_t pos = index[start];
  uint16_t steps = (uint16_t)hse->buffer[start];
  while (pos >= 0 && steps < 512) {
    steps = (uint16_t)(steps + 1);
    pos = index[(uint16_t)pos];
  }
  return (int16_t)steps;
}

/* Arm B: a member-array local whose struct-pointer root is SLICE-classified
 * (it escapes to win_off), so each use re-subscripts the root slice at the
 * root's own cursor before projecting the member. */
struct win {
  uint16_t base;
  int16_t ring[16];
};

static uint16_t win_off(struct win *w) { return w->base; }

static void win_mix(struct win *w) {
  int16_t *const ring = w->ring;
  const uint16_t off = win_off(w); /* slice-classifies the root */
  for (uint16_t i = 0; i < 16; i++)
    ring[i] = (int16_t)(ring[i] * 3 + off + i);
}

int main(int argc, char **argv) {
  uint16_t n = (uint16_t)(24 + argc * 3);
  struct enc *hse = malloc(sizeof(struct enc) + n);
  if (hse == NULL)
    return 1;
  hse->input_size = n;
  size_t index_sz = n * sizeof(uint16_t);
  hse->search_index = malloc(index_sz + sizeof(struct hs_index));
  if (hse->search_index == NULL) {
    free(hse);
    return 1;
  }
  hse->search_index->size = (uint16_t)index_sz;
  for (uint16_t i = 0; i < n; i++)
    hse->buffer[i] = (uint8_t)((i * 7 + argc * 13) & 0x0F);
  do_indexing(hse);
  long sum = 0;
  for (uint16_t i = 0; i < n; i++)
    sum += hse->search_index->index[i];
  printf("%ld %u\n", sum, (unsigned)hse->search_index->size);
  printf("%d %d\n", (int)chain_walk(hse, (uint16_t)(n - 1)),
         (int)chain_walk(hse, (uint16_t)(argc + 2)));
  free(hse->search_index);
  free(hse);

  struct win ws[2];
  for (uint16_t k = 0; k < 2; k++) {
    ws[k].base = (uint16_t)(argc * 5 + k);
    for (uint16_t i = 0; i < 16; i++)
      ws[k].ring[i] = (int16_t)(i * argc - k);
  }
  win_mix(&ws[argc & 1]); /* nonzero root cursor on the argc=1 path */
  win_mix(&ws[0]);
  long wsum = 0;
  for (uint16_t k = 0; k < 2; k++)
    for (uint16_t i = 0; i < 16; i++)
      wsum += ws[k].ring[i];
  printf("%ld\n", wsum);
  return 0;
}
