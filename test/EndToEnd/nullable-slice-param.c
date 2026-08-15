// REQUIRES: cargo
// FR-88: differential end-to-end test for NULLABLE byte-slice
// parameters (`Option<&[u8]>`). BOTH polarities of the guard are
// exercised on BOTH sides of the null: the positive-polarity
// guarded-memcpy shape (ctr_prng's `if (0 != p) memcpy(buf, p, len)`)
// and the negative-polarity cast-null early exit (hmac_prng/ccm's
// `if (p == (uint8_t *) 0 || ...) return;`). The argc-seeded branch
// selects the Some path (arguments present) or the None path (no
// arguments), so a missed fold bypass — the miscompile trap: folding a
// nullable parameter's null test to a constant — cannot hide: the None
// path would either panic (unwrap on None) or print the wrong guarded
// bytes, and the Some path pins the copied region bytes. All seeds
// derive from argc so constant folding cannot pre-compute the buffers;
// every output byte is printed. `cargo build` success alone proves
// nothing here — the stdout diff against the clang native is the
// oracle. Deterministic, no UB on either path.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/nullable_slice_param > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/nullable_slice_param a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Positive polarity: guarded memcpy from the nullable const byte param
   into a ui8 local buffer; the buffer (touched or untouched) is folded
   into the visible output. */
int init(uint8_t *out, const uint8_t *personalization, unsigned int plen) {
  uint8_t buf[16] = {0};
  if (0 != personalization) {
    unsigned int len = plen;
    if (len > sizeof buf) {
      len = sizeof buf;
    }
    memcpy(buf, personalization, len);
  }
  int sum = 0;
  unsigned int i;
  for (i = 0; i < 16u; i++) {
    out[i] = buf[i] ^ (uint8_t)i;
    sum += (int)out[i];
  }
  return sum;
}

/* Negative polarity: the corpus's cast-null early-exit sanity check;
   the region use sits AFTER the guard, proven non-null. */
int chk(const uint8_t *p, unsigned int len) {
  uint8_t tmp[4];
  if (p == (uint8_t *)0 || len < 4u) {
    return -1;
  }
  memcpy(tmp, p, 4);
  return (int)tmp[0] + (int)tmp[3];
}

int main(int argc, char **argv) {
  uint8_t seed[8];
  uint8_t out[16];
  unsigned int i;
  for (i = 0; i < 8u; i++) {
    seed[i] = (uint8_t)(argc * 17 + (int)i * 13);
  }
  int r;
  int c;
  if (argc > 1) {
    r = init(out, seed, 8u);
    c = chk(seed, 8u);
  } else {
    r = init(out, 0, 0u);
    c = chk(0, 8u);
  }
  for (i = 0; i < 16u; i++) {
    printf("%u ", (unsigned)out[i]);
  }
  printf("\nr=%d c=%d\n", r, c);
  return 0;
}
