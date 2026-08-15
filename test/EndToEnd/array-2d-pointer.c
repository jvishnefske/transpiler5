// REQUIRES: cargo
// FR-92: differential end-to-end test for the 2D-array-pointer cast
// shape — tiny-AES-c's remaining core (aes.c:413 Cipher + the
// aes.c:473/508/550 cast call sites). The `state_t*` family takes a
// whole-array `&mut [[u8; 4]; 4]`; Cipher FORWARDS its parameter to
// the family (index-free, aes.c:413), and the three cast source
// flavors all funnel through the on-demand `__emitrust_chunk_4x4`
// reshaping helper: a plain `uint8_t*` parameter (ECB), a WALKING
// cursor whose reslice must start at the CURRENT nonzero offset
// mid-loop (CBC — an off-by-a-block window ciphers the WRONG bytes
// and the diff catches it), and a local byte array (CTR). The family
// covers all three body index shapes (runtime `[i][j]`, constant
// `[c][c]`, mixed runtime-row/const-column) so a transposed or
// flattened-with-wrong-stride mapping changes bytes. All stored
// values derive from argc so constant folding cannot pre-compute the
// walk and hide a miscompile; the second RUN pair re-seeds via extra
// argv words. `cargo build` success alone proves nothing here — the
// stdout diff against the clang-built native is the oracle.
// Deterministic, no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/array_2d_pointer > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/array_2d_pointer a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>
#include <stdint.h>

typedef uint8_t state_t[4][4];

/* Runtime [i][j] double loop (the AddRoundKey/SubBytes shape). */
static void AddK(state_t* state, const uint8_t* rk, uint8_t round) {
  uint8_t i, j;
  for (i = 0; i < 4; ++i)
    for (j = 0; j < 4; ++j)
      (*state)[i][j] ^= rk[(round * 16u) + (i * 4u) + j];
}

/* Constant-index row/col swap (the ShiftRows shape). */
static void Rows(state_t* state) {
  uint8_t t = (*state)[0][0];
  (*state)[0][0] = (*state)[1][1];
  (*state)[1][1] = (*state)[2][2];
  (*state)[2][2] = (*state)[3][3];
  (*state)[3][3] = t;
}

/* Runtime-row, constant-column mix (the MixColumns shape). */
static void Mix(state_t* state) {
  uint8_t i;
  for (i = 0; i < 4; ++i) {
    uint8_t a = (*state)[i][0];
    (*state)[i][0] = (uint8_t)((*state)[i][1] ^ (uint8_t)(a << 1));
    (*state)[i][1] = (uint8_t)((*state)[i][2] ^ a);
    (*state)[i][2] = (uint8_t)((*state)[i][3] + a);
    (*state)[i][3] = (uint8_t)(a * 3u);
  }
}

/* Pure forward — never indexes state (the Cipher/InvCipher shape). */
static void Cipher(state_t* state, const uint8_t* rk) {
  uint8_t r;
  AddK(state, rk, 0);
  for (r = 1; r < 3; ++r) {
    Rows(state);
    Mix(state);
    AddK(state, rk, r);
  }
}

/* Flavor A: cast of a plain byte-slice parameter (aes.c:473 ECB). */
static void ecb(uint8_t* buf, const uint8_t* rk) {
  Cipher((state_t*)buf, rk);
}

/* Flavor B: cast of a WALKING cursor — the second iteration reslices
   at offset 16, so a window pinned to 0 diverges (aes.c:508 CBC). */
static void cbc(uint8_t* buf, unsigned len, const uint8_t* rk) {
  unsigned off;
  for (off = 0; off < len; off += 16) {
    Cipher((state_t*)buf, rk);
    buf += 16;
  }
}

/* Flavor C: cast of a local byte array (aes.c:550 CTR). */
static void ctr(const uint8_t* rk, uint8_t seed, uint8_t* out) {
  uint8_t buffer[16];
  uint8_t i;
  for (i = 0; i < 16; ++i) buffer[i] = (uint8_t)(seed + i * 7u);
  Cipher((state_t*)buffer, rk);
  for (i = 0; i < 16; ++i) out[i] ^= buffer[i];
}

int main(int argc, char** argv) {
  uint8_t rk[48];
  uint8_t buf[32];
  uint8_t i;
  for (i = 0; i < 48; ++i) rk[i] = (uint8_t)(argc * 31u + i * 5u);
  for (i = 0; i < 32; ++i) buf[i] = (uint8_t)(argc * 13u + i);
  ecb(buf, rk);
  cbc(buf, 32, rk);
  ctr(rk, (uint8_t)argc, buf);
  for (i = 0; i < 32; ++i) printf("%u ", buf[i]);
  printf("\n");
  return 0;
}
