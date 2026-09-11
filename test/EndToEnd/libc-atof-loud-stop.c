// REQUIRES: cargo
// FR-235, the other half of `libc-atof-special-forms.c`: THE LOUD STOP.
//
// Two shapes of C's strtod grammar have no Rust image. A hexadecimal float
// (`0x1p3`, and its exponent-less spelling `0X10`) is not accepted by
// `f64::from_str` and cannot be converted without reimplementing libc's
// rounding; a NaN PAYLOAD (`nan(1)`) maps its n-char sequence into the
// significand in a libc-specific way. Both used to return 0.0 SILENTLY.
// This project's documented safe direction is to stop loudly, so they
// panic -- with the same wording, in the same voice, as the `scanf %f`
// scanner that FR-229 settled first (`stdin-scan-float.c`), because the
// tree may have only ONE answer to this question.
//
// These cannot live in a byte-diff test: the native ANSWERS and the crate
// ABORTS, so no `diff` can hold them. Each sub-unit therefore pins BOTH
// sides separately -- the native's number, which is the divergence made
// visible rather than merely asserted, and the crate's exact panic text.
//
// The sign is consumed BEFORE the hex test, exactly as the scanf scanner
// does, so `-0x1p3` reaches the refusal too rather than falling through to
// the decimal scan and quietly answering 0.0; that is what `neghex` pins.
//
// Each subject is copied out of a table at an argc-derived row into a
// local buffer, so the panic is reached through a RUNTIME string. That is
// also the reason nothing is refused at import time: the divergence is a
// property of the string's contents, which at almost every real call site
// is a runtime value, and a rejection keyed on the literal sub-case would
// refuse a program whose `atof("0x1p3")` sits on a path that never runs
// while still missing every call that reads its argument from input.
//
// RUN: split-file %s %t
//
// 1: a hexadecimal float. C reads 8.0; the crate stops.
// RUN: emitrust-cc --emit=crate %t/hex.c -o %t/hex.crate --crate-name atof_hex --build
// RUN: clang -std=c11 -w %t/hex.c -o %t/hex.native
// RUN: %t/hex.native | FileCheck %s --check-prefix=HEXNATIVE
// HEXNATIVE: 8.000000
// RUN: not %t/hex.crate/target/release/atof_hex 2>&1 | FileCheck %s --check-prefix=HEXPANIC
// HEXPANIC: atof: hexadecimal floating-point input is not supported
//
// 2: the same behind a minus sign -- the refusal is after the sign scan.
// RUN: emitrust-cc --emit=crate %t/neghex.c -o %t/neghex.crate --crate-name atof_neghex --build
// RUN: clang -std=c11 -w %t/neghex.c -o %t/neghex.native
// RUN: %t/neghex.native | FileCheck %s --check-prefix=NEGNATIVE
// NEGNATIVE: -8.000000
// RUN: not %t/neghex.crate/target/release/atof_neghex 2>&1 | FileCheck %s --check-prefix=NEGPANIC
// NEGPANIC: atof: hexadecimal floating-point input is not supported
//
// 3: a NaN payload. C reads a NaN carrying 1 in its significand.
// RUN: emitrust-cc --emit=crate %t/payload.c -o %t/payload.crate --crate-name atof_payload --build
// RUN: clang -std=c11 -w %t/payload.c -o %t/payload.native
// RUN: %t/payload.native | FileCheck %s --check-prefix=PAYNATIVE
// PAYNATIVE: nan
// RUN: not %t/payload.crate/target/release/atof_payload 2>&1 | FileCheck %s --check-prefix=PAYPANIC
// PAYPANIC: atof: a NaN payload, nan(...), is not supported

//--- hex.c
#include <stdio.h>
#include <stdlib.h>

/* Row 0 is the refused form; row 1 exists only so the index is a real
   choice that neither compiler can fold away. argc is 1, so row 0 wins. */
static const char subject[2][8] = {"0x1p3", "1.5"};

int main(int argc, char **argv) {
  char buf[8];
  int j;
  for (j = 0; j < 8; j++) {
    buf[j] = subject[argc - 1][j];
  }
  printf("%.6f\n", atof(buf));
  return 0;
}

//--- neghex.c
#include <stdio.h>
#include <stdlib.h>

static const char subject[2][8] = {"-0x1p3", "1.5"};

int main(int argc, char **argv) {
  char buf[8];
  int j;
  for (j = 0; j < 8; j++) {
    buf[j] = subject[argc - 1][j];
  }
  printf("%.6f\n", atof(buf));
  return 0;
}

//--- payload.c
#include <stdio.h>
#include <stdlib.h>

static const char subject[2][8] = {"nan(1)", "1.5"};

int main(int argc, char **argv) {
  char buf[8];
  int j;
  for (j = 0; j < 8; j++) {
    buf[j] = subject[argc - 1][j];
  }
  printf("%.6f\n", atof(buf));
  return 0;
}
