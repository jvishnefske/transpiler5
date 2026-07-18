// REQUIRES: cargo
// CTS-P9 (00186): differential regression test for sprintf with literal
// formats. Covers the exact 00186 loop shape (zero-padded %02d formatted
// into a char array with an embedded newline, then re-printed through
// printf "%s"), width/left-align/zero-pad combos on negative and positive
// values (C's zero pad is sign-aware), hex/octal of -1 printing the
// two's-complement pattern, the returned length feeding arithmetic and
// later format arguments, and reformat-then-print reuse of one buffer.
// Byte-identical stdout and exit codes against the clang-built native
// binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name sprintf_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/sprintf_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdio.h>

int main(void) {
  char buf[64];
  int count;
  int total = 0;

  // The 00186 loop, verbatim shape: sprintf then printf("%s", buf).
  for (count = 1; count <= 20; count++) {
    sprintf(buf, "->%02d<-\n", count);
    printf("%s", buf);
  }

  // Width and flag forms on a negative value; the returned length is used.
  total = sprintf(buf, "[%5d][%-5d][%05d][%02d]", -42, -42, -42, -42);
  printf("%s|%d\n", buf, total);

  // Same forms on a positive value, accumulating the returned lengths.
  total = total + sprintf(buf, "[%5d][%-5d][%05d]", 7, 7, 7);
  printf("%s|%d\n", buf, total);

  // Hex/octal of -1 print the two's-complement bit pattern as unsigned.
  total = total + sprintf(buf, "%x %X %o %04X", -1, -1, 8, 255);
  printf("%s|%d\n", buf, total);

  // The accumulated length feeds later sprintf arguments.
  int n = sprintf(buf, "%d:%d", total, total * 2);
  printf("len=%d s=%s\n", n, buf);

  // Reformat-then-print: the same buffer is overwritten with a shorter
  // string and printed again (the NUL terminator must truncate the rest).
  sprintf(buf, "%02d", 5);
  printf("%s\n", buf);

  return 0;
}
