// REQUIRES: cargo
// FR-54 differential regression test for the narrowing cast of a shifted
// value — the core idiom of fixed-point DSP code, and the construct that
// used to abort the pipeline with "failed to legalize operation
// 'arith.shrui'".
//
// Two distinct lowering paths are exercised deliberately, because they are
// reached by different shift amounts:
//
//   * When the shift amount is a constant `c` no larger than
//     (source width - destination width), every bit that an arithmetic
//     shift would sign-fill is discarded by the truncation, so the
//     upstream Arith canonicalizer rewrites trunci(shrsi(x, c)) into
//     trunci(shrui(x, c)). That signless `arith.shrui` is the operation
//     the conversion now routes through the uN rendering.
//   * When the shift amount is larger, or is not a constant at all, the
//     canonicalizer cannot fire and the ARITHMETIC shift survives into the
//     narrowed result. Those cases are the ones where an arithmetic and a
//     logical shift disagree observably, so they are what would catch a
//     wrong-shift miscompile — `(int16_t)(-1 >> 20)` is -1 under an
//     arithmetic shift and 4095 under a logical one.
//
// The value set is chosen for that second property: every input is run at
// every shift amount from 0 to 31, and the inputs cover INT32_MIN, -1, the
// values straddling the 15/16-bit sign-bit boundaries in both directions,
// and INT32_MAX, so that both sides of every truncation boundary and both
// signs of every intermediate result are observed.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/shift_narrowing > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdint.h>

int printf(const char *, ...);

// Variable shift amount: the canonicalizer cannot fold it, so the
// arithmetic shift reaches the narrowing cast intact.
int16_t narrow16(int32_t v, int n) { return (int16_t)(v >> n); }
int8_t narrow8(int32_t v, int n) { return (int8_t)(v >> n); }
uint16_t narrow_u16(uint32_t v, int n) { return (uint16_t)(v >> n); }
int16_t narrow16_from_u32(uint32_t v, int n) { return (int16_t)((int32_t)v >> n); }

// Constant shift amounts at and around the canonicalization boundary for a
// 32 -> 16 narrowing (which is 16): 8 and 16 are rewritten to the logical
// shift, 20 and 24 keep the arithmetic shift.
int16_t q15_high(int32_t v) { return (int16_t)(v >> 16); }
int16_t narrow16_c8(int32_t v) { return (int16_t)(v >> 8); }
int16_t narrow16_c20(int32_t v) { return (int16_t)(v >> 20); }
int16_t narrow16_c24(int32_t v) { return (int16_t)(v >> 24); }
int8_t narrow8_c24(int32_t v) { return (int8_t)(v >> 24); }
int8_t narrow8_c28(int32_t v) { return (int8_t)(v >> 28); }

// 64 -> 32, the wide-accumulator form: 32 is the boundary, 40 is past it.
int32_t q31_high(long acc) { return (int32_t)(acc >> 32); }
int32_t narrow32_c40(long acc) { return (int32_t)(acc >> 40); }
int32_t narrow32(long acc, int n) { return (int32_t)(acc >> n); }

// The unsigned source is the control: it never becomes a signless shrui,
// because unsigned C arithmetic already lowers through the uN rendering.
uint16_t q15_high_u(uint32_t v) { return (uint16_t)(v >> 16); }
uint8_t narrow_u8_c20(uint32_t v) { return (uint8_t)(v >> 20); }

// The full-width shift with no narrowing at all, kept as a control that
// the arithmetic shift is untouched by the change.
int32_t full_shift(int32_t v, int n) { return v >> n; }
uint32_t full_shift_u(uint32_t v, int n) { return v >> n; }

int main(void) {
  int32_t values[16];
  values[0] = -2147483647 - 1; // INT32_MIN
  values[1] = -2147483647;
  values[2] = -1073741825;
  values[3] = -65537;
  values[4] = -65536;
  values[5] = -65535;
  values[6] = -32769;
  values[7] = -32768;
  values[8] = -1;
  values[9] = 0;
  values[10] = 1;
  values[11] = 32767;
  values[12] = 32768;
  values[13] = 65536;
  values[14] = 305419896;  // 0x12345678
  values[15] = 2147483647; // INT32_MAX

  for (int i = 0; i < 16; i++) {
    int32_t v = values[i];
    uint32_t u = (uint32_t)v;
    printf("const v=%d n16=%d c8=%d c20=%d c24=%d b24=%d b28=%d\n", v,
           (int)q15_high(v), (int)narrow16_c8(v), (int)narrow16_c20(v),
           (int)narrow16_c24(v), (int)narrow8_c24(v), (int)narrow8_c28(v));
    printf("constu u=%u hu=%u b20=%u\n", u, (unsigned)q15_high_u(u),
           (unsigned)narrow_u8_c20(u));
    for (int n = 0; n < 32; n++) {
      printf("var v=%d n=%d s16=%d s8=%d su16=%u sm=%d f=%d fu=%u\n", v, n,
             (int)narrow16(v, n), (int)narrow8(v, n),
             (unsigned)narrow_u16(u, n), (int)narrow16_from_u32(u, n),
             full_shift(v, n), full_shift_u(u, n));
    }
  }

  long wide[8];
  wide[0] = -9223372036854775807L - 1L; // INT64_MIN
  wide[1] = -4294967297L;
  wide[2] = -4294967296L;
  wide[3] = -1L;
  wide[4] = 0L;
  wide[5] = 1L;
  wide[6] = 4294967296L;
  wide[7] = 9223372036854775807L; // INT64_MAX

  for (int i = 0; i < 8; i++) {
    long a = wide[i];
    printf("wide a=%ld hi=%d c40=%d\n", a, q31_high(a), narrow32_c40(a));
    for (int n = 0; n < 64; n++) {
      printf("widevar a=%ld n=%d s32=%d\n", a, n, narrow32(a, n));
    }
  }
  return 0;
}
