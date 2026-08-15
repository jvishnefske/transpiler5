// REQUIRES: cargo
// FR-94 differential end-to-end test: a flexible-array-member record with an
// allocation-backed tail — the reduced heatshrink_decoder lifecycle. All five
// heatshrink FAM site shapes are exercised over ONE record with runtime sizes
// seeded from argc (so no size can fold at import time):
//   - alloc:    malloc(sizeof(S) + n), n = (1 << w) + ibs   (decoder.c:49-70)
//   - reset:    memset(d->buffers, 0, n)                    (decoder.c:83)
//   - sink:     memcpy(&d->buffers[d->input_size], in, len) (decoder.c:110)
//   - poll:     uint8_t *buf = &d->buffers[ibs]; buf[i] r/w (decoder.c:208,268)
//   - get_bits: d->buffers[d->input_index++]                (decoder.c:309)
//   - free(d)                                               (decoder.c:72-77)
// The record imports with an owned Vec<u8> tail (non-Copy struct: the derive
// drops Copy), the alloc function returns the record BY VALUE, the free
// wrapper consumes it BY VALUE, and every access above lowers over the Vec
// member. The crate's stdout must byte-match the natively compiled C program
// for argc = 1 and argc = 4 — a divergence in the tail extent, the member
// offset arithmetic, or the window cursor shifts a printed byte.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n1.out
// RUN: %t.crate/target/release/flexible_array_owned_tail > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b c > %t.n4.out
// RUN: %t.crate/target/release/flexible_array_owned_tail a b c > %t.r4.out
// RUN: diff %t.n4.out %t.r4.out

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  unsigned short input_size;
  unsigned short input_index;
  unsigned char window_sz2;
  unsigned short input_buffer_size;
  unsigned char buffers[];
} dec;

dec *dec_alloc(unsigned char w, unsigned short ibs) {
  size_t n = ((size_t)1 << w) + ibs;
  dec *d = malloc(sizeof(dec) + n);
  if (!d) return NULL;
  d->input_size = 0;
  d->input_index = 0;
  d->window_sz2 = w;
  d->input_buffer_size = ibs;
  memset(d->buffers, 0, n);
  return d;
}

void dec_free(dec *d) { free(d); }

void dec_sink(dec *d, const unsigned char *in, unsigned len) {
  memcpy(&d->buffers[d->input_size], in, len);
  d->input_size += len;
}

unsigned char dec_next(dec *d) {
  return d->buffers[d->input_index++];
}

unsigned dec_window_sum(dec *d) {
  unsigned char *buf = &d->buffers[d->input_buffer_size];
  size_t wsz = (size_t)1 << d->window_sz2;
  unsigned s = 0;
  for (size_t i = 0; i < wsz; i++) {
    buf[i] = (unsigned char)(buf[i] + (unsigned char)i + 3u);
    s += buf[i];
  }
  return s;
}

int main(int argc, char **argv) {
  unsigned char w = (unsigned char)(4 + argc);     /* runtime window bits */
  unsigned short ibs = (unsigned short)(8 * argc); /* runtime input size */
  dec *d = dec_alloc(w, ibs);
  if (!d) return 1;
  unsigned char msg[8] = {11, 22, 33, 44, 55, 66, 77, 88};
  dec_sink(d, msg, (unsigned)(argc + 3));
  dec_sink(d, msg, 4u);
  unsigned acc = 0;
  while (d->input_index < d->input_size)
    acc = acc * 31u + dec_next(d);
  unsigned ws = dec_window_sum(d);
  printf("w=%u ibs=%u acc=%u ws=%u last=%u\n", (unsigned)d->window_sz2,
         (unsigned)d->input_buffer_size, acc, ws,
         (unsigned)d->buffers[d->input_buffer_size]);
  dec_free(d);
  return 0;
}
