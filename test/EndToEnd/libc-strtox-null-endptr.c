// REQUIRES: cargo
// FR-234 rung 1: `strtol`, `strtoul` and `strtod` over a char region with a
// NULL `endptr`, byte-diffed against the clang-built native. Before this
// change all three were a flat system-header refusal
// (`call to 'strtol' declared in a system header`), so there is no prior
// behaviour to regress -- what this file exists to stop is the OTHER
// direction: a plausible-looking parse that is off by a bit somewhere in
// C's PREFIX grammar. `cargo build` cannot see that and neither can any
// FileCheck over the emitted Rust; only this diff can.
//
// WHAT IS PINNED. Every one of these is a place a from-scratch integer
// parser gets it wrong, and each is a row (or a column) below:
//   1 THE PREFIX RULE. strtol converts the LONGEST INITIAL SUBSEQUENCE of
//     the expected form and ignores the rest: `42x` is 42, `  \t+0X10p`
//     is 16 in base 16, `1_000` is 1. `x42` and `--1` convert nothing.
//   2 LEADING WHITESPACE is the full C-locale set, not just the space:
//     `\t\n\v\f\r ` all precede a subject sequence (row 5).
//   3 BASE 0 AUTO-DETECTION: `0x`/`0X` + a hex digit is 16, an otherwise
//     leading `0` is 8, everything else is 10. `010` is 8 in base 0 and 10
//     in base 10; `0777` is 511 and `08` is 0 (the `8` is not an octal
//     digit, so the subject sequence is just the `0`).
//   4 THE `0x` PREFIX IS CONDITIONAL. `0x` with no hex digit after it is
//     NOT a prefix -- in base 0 and base 16 alike the subject sequence is
//     the single `0` and the answer is 0, not "no conversion". Rows 12
//     (`0x`) and 15 (`0xg`) are the ones an unconditional two-character
//     skip gets wrong.
//   5 LETTER DIGITS in both cases across the whole base range: `z`/`Z` is
//     35 in base 36 and no conversion in base 10; `7fffffffffffffff` and
//     `ffffffffffffffff` are the signed and unsigned extremes in base 16.
//   6 OVERFLOW SATURATES IN BOTH DIRECTIONS, and the saturation is
//     type-directed, not shared: strtol clamps to LONG_MAX/LONG_MIN and
//     strtoul to ULONG_MAX, so the SAME input string prints two different
//     answers in the `L` and `U` columns. Rows 25/27 (one past each
//     signed extreme), 29/32 (one past each unsigned extreme) and 33/34
//     (far past every extreme) carry it.
//   7 `strtoul` OF A NEGATIVE IS DEFINED and is NOT saturation: it
//     converts the magnitude and negates it modulo 2^64, so `-1` is
//     ULONG_MAX while `-18446744073709551616` -- which overflows first --
//     is ALSO ULONG_MAX, for a different reason. Rows 30 and 32 separate
//     the two. `-0` (row 22) stays 0 rather than becoming ULONG_MAX.
//   8 NO CONVERSION returns 0 and never reads past the subject: the empty
//     string, a lone space, a lone sign.
//   9 AN OUT-OF-RANGE BASE (columns 6 and 7: 1 and 37) is UNDEFINED in C
//     7.22.1.4p2. It is here as a PLATFORM REFINEMENT PIN, not a
//     conformance claim: the emitted crate answers 0 and converts
//     nothing, which is what this differential oracle's glibc does, so
//     the row is evidence about THIS platform only. A reader ranking a
//     future change must not read these two columns as portable C.
//  10 strtod IS atof. C 7.22.1.1p2 defines `atof(s)` as `strtod(s, NULL)`,
//     so this lowering reuses `__emitrust_atof` VERBATIM rather than
//     growing a second float grammar -- the tree keeps one answer to the
//     question FR-235 settled. The `D` rows add what the atof tests do
//     not reach: ERANGE OVERFLOW to an infinity (`1e400`), ERANGE
//     UNDERFLOW to zero (`1e-400`), and the two subnormal extremes, where
//     a parser that is merely "close" diverges in the last bit and
//     `%.17g` shows it.
//
// NOT here, and it CANNOT be here: the hexadecimal-float and NaN-payload
// forms of strtod PANIC (FR-235), so the native answers and the crate
// aborts and no byte-diff can hold them; they stay pinned in
// `libc-atof-loud-stop.c`, and this file's `D` table deliberately omits
// them. Also not here: a non-NULL `endptr`, which is FR-234 rung 2 and is
// a LOCATED REJECTION today -- pinned in
// `test/Import/C/libc-strtox-invalid.c`.
//
// NOT OBSERVABLE, AND SCOPED OUT ON PURPOSE: `errno`. The importer models
// no `errno` at all (a reference to it is a system-header rejection), so
// the ERANGE half of C's overflow contract has no image here. What IS
// observable is the RETURNED VALUE, which is what rows 25-34 pin.
//
// Every subject string is copied out of the table into a local buffer at a
// row index derived from argc, and the base is read at an argc-derived
// index too, so neither compiler is handed a constant argument and no
// folded literal can stand in for a working parse. With argc == 1 both
// offsets are 0, so all 46 x 8 x 2 + 12 lines are still produced.
// Deterministic, no UB in the C itself apart from the two base columns
// called out in 9 above; main returns 0.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name libc_strtox_null_endptr --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/libc_strtox_null_endptr > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdio.h>
#include <stdlib.h>

#define NFORMS 46
#define WIDTH 26
#define NBASES 8

/* NUL-padded rows: every row is WIDTH bytes, so copying the whole row
   always yields a NUL-terminated buffer. */
static const char forms[NFORMS][WIDTH] = {
    "0",
    "42",
    "-42",
    "+42",
    "  42",
    "\t\n\v\f\r 42",
    "42x",
    "x42",
    "",
    " ",
    "-",
    "+",
    "0x",
    "0x1f",
    "0X1F",
    "0xg",
    "010",
    "-010",
    "0b101",
    "z",
    "Z",
    "zz",
    "-0",
    "-0x10",
    "9223372036854775807",
    "9223372036854775808",
    "-9223372036854775808",
    "-9223372036854775809",
    "18446744073709551615",
    "18446744073709551616",
    "-1",
    "-18446744073709551615",
    "-18446744073709551616",
    "99999999999999999999999",
    "-99999999999999999999999",
    "7fffffffffffffff",
    "ffffffffffffffff",
    "1010",
    "zzzzzzzzzzzzz",
    "  \t+0X10p",
    "0777",
    "08",
    "  ",
    "1_000",
    "--1",
    "+-1"};

static const int bases[NBASES] = {0, 2, 8, 10, 16, 36, 1, 37};

#define NDFORMS 12
static const char dforms[NDFORMS][WIDTH] = {
    "1e400",
    "-1e400",
    "1e-400",
    "5e-324",
    "-5e-324",
    "2.2250738585072011e-308",
    "1.7976931348623157e309",
    "0.1",
    "  -42.5abc",
    "3.5xyz",
    "1e308",
    "inf"};

int main(int argc, char **argv) {
  char buf[WIDTH];
  int i;
  int j;
  int k;
  int b;

  for (i = 0; i < NFORMS; i++) {
    for (j = 0; j < WIDTH; j++) {
      buf[j] = forms[i + argc - 1][j];
    }
    for (k = 0; k < NBASES; k++) {
      b = bases[(k + argc - 1) % NBASES];
      printf("L %02d %02d %ld\n", i, k, strtol(buf, NULL, b));
      printf("U %02d %02d %lu\n", i, k, strtoul(buf, NULL, b));
    }
  }

  for (i = 0; i < NDFORMS; i++) {
    for (j = 0; j < WIDTH; j++) {
      buf[j] = dforms[i + argc - 1][j];
    }
    printf("D %02d %.17g\n", i, strtod(buf, NULL));
  }
  return 0;
}
