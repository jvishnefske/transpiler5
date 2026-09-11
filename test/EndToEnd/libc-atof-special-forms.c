// REQUIRES: cargo
// FR-235: the NON-DECIMAL half of C's strtod grammar, byte-diffed against
// the clang-built native. This file exists because of a MEASURED silent
// miscompile, not a hypothesis: before this change the emitted crate
// answered 0.0 for `atof("inf")`, `atof("nan")` and `atof("0x1p3")` where
// C answers infinity, a NaN and 8.0 -- exit 0, no diagnostic, 21 of the 44
// vectors below wrong. `cargo build` cannot see that and neither can any
// FileCheck over the emitted Rust; only this diff can.
//
// WHAT IS PINNED -- the forms that must MATCH C exactly:
//   1 `inf` and `infinity` in every case mixture, with either sign and
//     with leading whitespace. IEEE-754 fixes every bit of an infinity, so
//     agreement here is exact rather than approximate and `%.17g` shows it.
//   2 strtod's LONGEST-PREFIX rule over those keywords, which is exactly
//     where it parts company with `scanf`'s non-backtracking scanner
//     (FR-229): strtod has the whole string in hand and backtracks, so
//     `infi`, `infin`, `infinit` and `infx` are ALL `inf` (measured
//     against glibc), while `in` and `i` carry no numeric prefix at all
//     and are 0.0. A scanner that consumed `inf` greedily and then failed
//     would differ on four of these six.
//   3 `nan` in every case mixture, either sign, and `nanQ`. Pinned by the
//     PREDICATE only: a NaN's payload and sign bit are not C-defined, and
//     this oracle compares against the HOST libc, so asserting the bytes
//     would be asserting a glibc coincidence. (They do in fact agree --
//     glibc's `-nan` is fff8000000000000 and Rust's `-f64::NAN` is
//     fff8000000000000 -- which is why the sign is carried, not dropped.)
//   4 `0xyz`, `0x`, `0X` and glibc's degenerate `0x.p3`: strings that begin
//     `0x` but where C's HEXADECIMAL form does not apply, because there is
//     no hex digit. C reads the prefix `0` and answers 0.0, and so must
//     this -- these must NOT be swept up by the hex refusal. This is the
//     one place the helper is deliberately more precise than the scanf
//     scanner, which has only one character of pushback and must fire on
//     `0x` alone.
//   5 that everything already correct is UNCHANGED: the whitespace-and-
//     trailing-junk shape `"  12.5xyz"`, both signs, a bare fraction, a
//     trailing dot, an `e` with no exponent digits, an explicit exponent,
//     a trailing newline, negative zero, and the no-valid-prefix cases
//     (`abc`, the empty string) -> 0.0.
//
// NOT here, and it CANNOT be here: `0x1p3` and `nan(1)`. Those PANIC, so
// the native prints a number while the crate aborts and no byte-diff can
// hold them by construction. They are pinned in `libc-atof-loud-stop.c`.
//
// Every subject string is copied out of the table into a local buffer at a
// row index derived from argc, so neither compiler is handed a constant
// string and no folded literal can stand in for a working parse. With
// argc == 1 the offset is 0, so all 44 rows are still visited.
// Deterministic, no UB; main returns 0.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name libc_atof_special_forms --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/libc_atof_special_forms > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdio.h>
#include <stdlib.h>

#define NFORMS 44
#define WIDTH 16

/* NUL-padded rows: every row is WIDTH bytes, so copying the whole row
   always yields a NUL-terminated buffer. */
static const char forms[NFORMS][WIDTH] = {
    "inf",       "INF",         "Inf",         "infinity",
    "INFINITY",  "iNfInItY",    "-inf",        "+inf",
    "infi",      "infin",       "infinit",     "infx",
    "in",        "i",           "-INFINITY",   "  inf",
    "\tinfinity", "nan",        "NAN",         "NaN",
    "-nan",      "+nan",        "nanQ",        "na",
    "n",         "0xyz",        "0x",          "0x.p3",
    "0",         "0X",          "  12.5xyz",   "3.0",
    "-0.0",      "abc",         "",            "2.5\n",
    "  -42.5abc", "   +1.25e2xyz", ".5",       "5.",
    "1e",        "12e+3",       "-.75",        "0.1"};

/* C's `isnan` without <math.h>: only a NaN is unequal to itself. The
   comparison goes through a function so the emitted Rust does not spell
   `d != d`, which would manufacture a `clippy::eq_op`. */
static int unordered(double a, double b) { return a != b; }

int main(int argc, char **argv) {
  char buf[WIDTH];
  int i;
  int j;
  double d;

  for (i = 0; i < NFORMS; i++) {
    for (j = 0; j < WIDTH; j++) {
      buf[j] = forms[i + argc - 1][j];
    }
    d = atof(buf);
    if (unordered(d, d)) {
      printf("%02d nan\n", i);
    } else {
      printf("%02d %.17g\n", i, d);
    }
  }
  return 0;
}
