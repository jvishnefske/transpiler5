// REQUIRES: cargo
// FR-224: differential regression for the libc SHIM TABLE -- atof,
// strcspn/strspn, div, fputs and setlocale. What this file pins is that
// each shim reproduces the C library function's OBSERVABLE result, byte
// for byte, on the shapes the importer admits; the emitted crate's
// stdout is diffed against the clang-built native's and nothing less
// counts.
//
// THE ONE THAT IS NOT OBVIOUS IS atof. C's atof is `strtod(s, NULL)`,
// which is a PREFIX parse: it skips leading whitespace, consumes the
// longest initial subsequence of the number grammar, and ignores
// whatever follows -- so `atof("  -42.5abc")` is -42.5 and
// `atof("abc")` is 0.0. Rust's `str::parse::<f64>` does none of that: it
// demands the WHOLE string, rejects leading whitespace, and rejects the
// trailing newline that every `fgets` leaves behind, which is exactly
// the shape the motivating corpus case feeds it. The shim therefore
// scans the prefix itself and parses only that; both sides are then
// CORRECTLY ROUNDED (glibc's strtod is, Rust's from_str is), and two
// correctly-rounded parses of the same digits are identical by
// definition. `%.17g` is used so a last-bit disagreement cannot hide.
//
// NOT TESTED HERE, DELIBERATELY: values too large to represent
// (`atof("1e400")`) are explicitly UNDEFINED for the atoi/atof family
// (C11 7.22.1p1), so the two builds are permitted to differ and a test
// asserting agreement would be asserting a coincidence. Likewise
// `div(INT_MIN, -1)`, whose quotient is not representable.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name libc_shim_table_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/libc_shim_table_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void show_atof(const char *s) {
  printf("atof %.17g\n", atof(s));
}

int main(void) {
  /* setlocale to the startup locale "C" is the empty call: everything
     after it must be identical to a program that never made it. */
  setlocale(LC_ALL, "C");

  /* atof: the prefix grammar, both signs, a bare fraction, a bare
     integer part with a trailing dot, an exponent, an exponent marker
     with no digits after it (the 'e' is NOT consumed), leading junk
     (0.0), the empty string (0.0), and -- the corpus shape -- a
     trailing newline. */
  show_atof("3.0");
  show_atof("4.0");
  show_atof("0.0");
  show_atof("-0.0");
  show_atof("  -42.5abc");
  show_atof("   +1.25e2xyz");
  show_atof(".5");
  show_atof("5.");
  show_atof("1e");
  show_atof("12e+3");
  show_atof("-.75");
  show_atof("abc");
  show_atof("");
  show_atof("2.5\n");
  show_atof("\t\r\n 7.125");
  show_atof("0.1");
  show_atof("1.7976931348623157");

  /* strcspn/strspn over both a literal and a mutable char array: no
     match (the whole length), a match at index 0, a match in the middle,
     an empty reject/accept set, and an empty subject. */
  char subject[16] = "hello world";
  printf("cspn %zu %zu %zu %zu %zu\n", strcspn(subject, "xyz"),
         strcspn(subject, "h"), strcspn(subject, " "), strcspn(subject, ""),
         strcspn("", "abc"));
  printf("spn %zu %zu %zu %zu %zu\n", strspn(subject, "hel"),
         strspn(subject, "xyz"), strspn(subject, "helo wrd"),
         strspn(subject, ""), strspn("", "abc"));
  /* Both arguments may name the SAME object: two shared borrows. */
  printf("same %zu %zu\n", strcspn(subject, subject), strspn(subject, subject));

  /* div: C's truncating quotient and the remainder that completes the
     identity quot*denom+rem == numer, over all four sign combinations
     plus an exact division. */
  int numers[6] = {7, -7, 7, -7, 8, 0};
  int denoms[6] = {2, 2, -2, -2, 4, 5};
  int i;
  for (i = 0; i < 6; i++) {
    div_t d = div(numers[i], denoms[i]);
    printf("div %d %d -> %d %d\n", numers[i], denoms[i], d.quot, d.rem);
  }
  /* ldiv shares the lowering at long width. */
  ldiv_t l = ldiv(-1000000007L, 3L);
  printf("ldiv %ld %ld\n", l.quot, l.rem);

  /* fputs writes the bytes and NO newline (contrast puts). The high-byte
     buffer is the FR-191 shape: a char region routed through a Latin-1
     Display funnel would print TWO UTF-8 bytes where C writes one, so
     this line is where that defect would show as a diff. */
  fputs("no", stdout);
  fputs("newline", stdout);
  fputs("\n", stdout);
  char raw[5];
  raw[0] = (char)0xc8;
  raw[1] = (char)0xff;
  raw[2] = 'z';
  raw[3] = (char)0x80;
  raw[4] = '\0';
  fputs(raw, stdout);
  fputs("\n", stdout);
  fputs("", stdout);
  puts("end");
  return 0;
}
