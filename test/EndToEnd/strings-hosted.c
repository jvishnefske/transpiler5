// REQUIRES: cargo
// C99-48 / CTS-L1: differential regression test for the hosted <string.h>
// subset. strcpy copies a literal into a char array (including through the
// NUL) and the array prints back through %s; strncpy stops mid-source
// without a terminator and NUL-pads a longer count; strcat appends at the
// destination NUL; strcmp/strncmp/memcmp compare as unsigned char in both
// orders and on equality; strlen counts a char array; strchr/strrchr
// results feed %s (from a mid-array cursor too) and null comparisons in
// both found and not-found forms; memset fills through an element pointer;
// memcpy covers both the distinct-array and the same-array
// (borrow-splitting copy_within) shapes. Byte-identical stdout and exit
// codes against the clang-built native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name strings_hosted_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/strings_hosted_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdio.h>
#include <string.h>

int main(void) {
  char a[16];
  char b[16];

  // strcpy: literal into array, then array reads back whole and offset.
  strcpy(a, "hello world");
  printf("cpy %s|%s\n", a, &a[6]);

  // strcpy from another array (distinct objects).
  strcpy(b, a);
  printf("cpy2 %s\n", b);

  // strncpy short count: no terminator is written past the count, so the
  // tail of the previous contents shows through.
  strncpy(a, "HERD", 2);
  printf("ncpy %s\n", a);
  // strncpy long count: the source ends early and the rest is NUL-padded.
  strncpy(b, "pad", 6);
  printf("ncpy2 %s %d\n", b, (int)b[5]);

  // strcat appends at the destination's NUL.
  strcpy(a, "go");
  strcat(a, "!");
  printf("cat %s\n", a);

  // Comparisons: sign in both directions and equality, full and counted.
  printf("cmp %d %d %d\n", strcmp(a, "apple") > 0, strcmp(a, "zebra") < 0,
         strcmp(a, "go!") == 0);
  printf("ncmp %d %d %d\n", strncmp(a, "gz", 1) == 0, strncmp(a, "ga", 2) > 0,
         strncmp(a, "gz", 2) < 0);
  printf("len %d %d\n", (int)strlen(a), (int)strlen(b));

  // strchr/strrchr: found (printed via %s) and not-found (NULL compares).
  strcpy(a, "banana");
  printf("chr %s %s\n", strchr(a, 'n'), strrchr(a, 'n'));
  printf("chr2 %d %d %d %d\n", strchr(a, 'q') == NULL, strchr(a, 'b') != NULL,
         strrchr(a, 'q') == NULL, strrchr(a, 'a') != NULL);
  // Search from a mid-array cursor.
  printf("chr3 %s\n", strchr(&a[2], 'n'));

  // memset through an element pointer, partial fill.
  memset(&a[1], 'x', 3);
  printf("set %s\n", a);

  // memcpy between distinct arrays, then within one array (the same-base
  // shape lowers to a single mutable borrow plus two cursors).
  memcpy(b, a, 4);
  b[4] = 0;
  printf("mcpy %s\n", b);
  memcpy(&a[4], a, 2);
  printf("mcpy2 %s\n", a);

  // memcmp: equal prefix, then differing byte in both directions.
  printf("mcmp %d %d %d\n", memcmp(a, a, 6) == 0, memcmp("ab", "ac", 2) < 0,
         memcmp("ad", "ac", 2) > 0);

  return strcmp(a, "") == 0;
}
