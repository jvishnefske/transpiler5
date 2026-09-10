// REQUIRES: cargo
// FR-183's float flip: the `%f` family over `stdin`, byte-diffed against the
// clang-built native. THE reason this file exists rather than a golden is
// that the two corpus programs which need `%f` print the float's RAW BYTES,
// so a one-ulp difference between glibc's `strtof` and Rust's parser would
// be a visible miscompile that no `cargo build` and no FileCheck could see.
// Every value below is therefore shown by its raw bytes too; a `printf("%f")`
// would round the evidence away.
//
// The entropy is standard input, which neither compiler can constant-fold.
//
// WHAT IS PINNED, and why each vector is in C-DEFINED territory (the same
// portability rule as `stdin-scan.c`: this oracle compares against the HOST
// libc, so a glibc-only quirk would fail elsewhere):
//   1 the six corpus vectors verbatim, including `-0` -> NEGATIVE zero and
//     an exponent form;
//   2 bit-exactness at the hard end of the range -- the f32/f64 subnormal
//     boundary, the round-to-even case 16777217, FLT_MAX, and decimal
//     strings far longer than the significand;
//   3 the greedy scanner's failure edges: C reads the LONGEST prefix of a
//     matching sequence and never backtracks, so `1.2e+x` is a matching
//     failure with only `x` left unread;
//   4 `%e`, `%g` and `%a` reading the same syntax as `%f`, `%lf` reading it
//     into a `double`, and a float directive threading the state in one
//     format with `%d` and `%c`;
//   5 `inf`/`infinity` in mixed case (IEEE fixes those bits exactly).
// NaN's PAYLOAD is not C-defined, so the last line pins only the predicate.
//
// NOT pinned here, deliberately: C's hexadecimal form (`0x1p3`) and a NaN
// payload (`nan(1)`). Rust's parser accepts neither, they arrive from
// RUNTIME input so no import-time rejection can catch them, and this project
// stops loudly rather than answering a different number -- see the panic
// pinned at the end of this file.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name stdin_scan_float_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
//
// 1: the corpus vectors, then the bit-exactness battery.
// RUN: printf '123 -123 0 -0 1.234 1.234e6 0.1 1e-45 1e-46 16777217 3.4028235e38 1.1754943e-38 0.1 1e-300 2.2250738585072014e-308 4.9406564584124654e-324 1.7976931348623157e308 -0.0 1.5e3 2.5 7.25 42 3.5X nan\n' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/stdin_scan_float_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// 2: long decimal strings, huge and tiny exponents, and the forms with an
// empty integer or empty fraction part.
// RUN: printf '1e999999 -1e999999 1e-999999 -1e-999999 00000000000000000000000000000000000000000001 1.00000000000000000000000000000000000000001 .5 5. +1.5 -.5 1.e5 16777216 16777219 1e38 1e39 1e-300 1e-320 1e308 1e309 0.30000000000000004 1.5 2.5 7.5 3 4.5Y nan\n' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/stdin_scan_float_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// 3: a matching failure on the very first conversion -- the value is left
// untouched and the offending byte is still there for the next reader.
// RUN: printf 'abc\n' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/stdin_scan_float_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// 4: the greedy-prefix failures. `1.2e+x` has consumed `1.2e+` by the time
// it fails, so exactly `x` is what the following `getchar` sees.
// RUN: printf '1.2e+x . -. + --5 1..2 1d5 e5\n' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/stdin_scan_float_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// 5: end of file before anything -- an input failure, -1, not 0.
// RUN: printf '' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/stdin_scan_float_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// 6: the infinity spellings, in every case mixture.
// RUN: printf 'inf -inf INF -INF infinity -infinity INFINITY iNfInItY Inf +inf inf inf 1 2 3 4 5 6 7 8 9 10 11X nan\n' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/stdin_scan_float_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// 7: THE LOUD STOP. A hexadecimal float reaches the reader only at runtime,
// so it cannot be an import-time rejection; it must not be a silently
// different number either. `not ... | FileCheck` pins the panic wording.
// RUN: printf '0x1p3\n' > %t.in
// RUN: not %t.crate/target/release/stdin_scan_float_e2e < %t.in 2>&1 | FileCheck %s --check-prefix=HEXPANIC
// HEXPANIC: scanf: hexadecimal floating-point input is not supported
//
// RUN: printf 'nan(1)\n' > %t.in
// RUN: not %t.crate/target/release/stdin_scan_float_e2e < %t.in 2>&1 | FileCheck %s --check-prefix=NANPANIC
// NANPANIC: scanf: a NaN payload, nan(...), is not supported

#include <stdio.h>

/* The corpus's own byte view: this is what makes the oracle bit-exact. */
static void print_hex(unsigned char *p, int len) {
  for (int i = 0; i < len; i++) {
    printf("%02x", p[i]);
  }
  printf("\n");
}

/* C's `isnan` without <math.h>: only a NaN is unequal to itself. */
static int unordered(float a, float b) { return a != b; }

int main(void) {
  float x = 42.0f;
  double d = 42.0;
  int n = -1;
  char ch = '?';
  int r = 0;
  int i;
  int c;

  for (i = 0; i < 12; i++) {
    r = scanf("%f", &x);
    printf("f%d r=%d ", i, r);
    print_hex((unsigned char *)&x, sizeof(x));
    if (r != 1)
      break;
  }

  /* Whatever byte the last directive stopped on is still unread. */
  c = getchar();
  printf("next=%d\n", c);

  for (i = 0; i < 6; i++) {
    r = scanf("%lf", &d);
    printf("d%d r=%d ", i, r);
    print_hex((unsigned char *)&d, sizeof(d));
    if (r != 1)
      break;
  }

  /* %e, %g and %a are `%f` in C's scanf grammar: same input syntax, same
     argument type. */
  r = scanf("%e", &x);
  printf("e r=%d ", r);
  print_hex((unsigned char *)&x, sizeof(x));
  r = scanf("%g", &x);
  printf("g r=%d ", r);
  print_hex((unsigned char *)&x, sizeof(x));
  r = scanf("%a", &x);
  printf("a r=%d ", r);
  print_hex((unsigned char *)&x, sizeof(x));

  /* A float directive threads the chain's state exactly like an integer
     one, and `%c` still reads the byte the float stopped on. */
  r = scanf("%d %f%c", &n, &x, &ch);
  printf("mix r=%d n=%d ch=%d ", r, n, (int)ch);
  print_hex((unsigned char *)&x, sizeof(x));

  /* A NaN's bits are not C-defined; only that it IS one. The comparison
     goes through a function so the emitted Rust does not spell `x != x`,
     which would manufacture a `clippy::eq_op` in a crate this project
     scores for lints. */
  r = scanf("%f", &x);
  printf("nanr=%d isnan=%d\n", r, unordered(x, x));
  return 0;
}
