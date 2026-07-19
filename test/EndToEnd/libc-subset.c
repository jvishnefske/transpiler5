// REQUIRES: cargo
// C99-48: differential regression test for the curated stdlib/string/math
// additions to the hosted libc subset. memmove covers the distinct-region
// shape and both overlap directions in one array (copy_within); abs/labs
// cover negative, positive, and zero; atoi covers C's exact semantics —
// leading whitespace, both signs, stop at the first non-digit, leading
// junk and the empty string yielding 0, and leading zeros; the empty
// string, equal, and prefix compares pin the string.h edges; the
// IEEE-exact fabs/sqrt/floor/ceil run on positive, negative, zero, and
// exact-integer inputs; and the final exit(7) pins C's exit-status
// semantics. Byte-identical stdout AND exit status against the
// clang-built native binary are required (the rc= line records $?).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name libc_subset_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native -lm
// RUN: sh -c '%t.native > %t.native.out; echo rc=$? >> %t.native.out'
// RUN: sh -c '%t.crate/target/release/libc_subset_e2e > %t.rust.out; echo rc=$? >> %t.rust.out'
// RUN: diff %t.native.out %t.rust.out

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sign(int x) { return (x > 0) - (x < 0); }

int main(void) {
  char buf[16];

  // memmove between distinct regions.
  strcpy(buf, "abcdefgh");
  memmove(buf, "XY", 2);
  printf("mv %s\n", buf);

  // Overlapping memmove, forward (dst > src) and backward (dst < src).
  strcpy(buf, "abcdef");
  memmove(&buf[2], buf, 3);
  printf("fw %s\n", buf);
  strcpy(buf, "abcdef");
  memmove(buf, &buf[2], 3);
  printf("bw %s\n", buf);

  // abs/labs: negative, positive, zero.
  printf("abs %d %d %d\n", abs(-5), abs(5), abs(0));
  printf("labs %ld %ld %ld\n", labs(-6L), labs(6L), labs(0L));

  // atoi: C's exact parse. Leading whitespace and signs; stop at the
  // first non-digit; leading junk and the empty string yield 0; leading
  // zeros are decimal.
  char num[8] = " \t-123x";
  printf("a1 %d\n", atoi(num));
  printf("a2 %d\n", atoi("  +42"));
  printf("a3 %d\n", atoi("x42"));
  printf("a4 %d\n", atoi(""));
  printf("a5 %d\n", atoi("   "));
  printf("a6 %d\n", atoi("007"));
  printf("a7 %d\n", atoi("2147483647"));
  printf("a8 %d\n", atoi("-2147483648"));
  // atoi from a mid-region cursor skips the sign it starts past.
  printf("a9 %d\n", atoi(&num[3]));

  // Empty-string, equal, and prefix compares (sign-normalized: C pins
  // only the sign of strcmp results).
  char empty[1] = "";
  printf("s1 %d %d\n", (int)strlen(empty), sign(strcmp(empty, "")));
  printf("s2 %d %d\n", sign(strcmp("abc", "abc")), sign(strncmp("abc", "abx", 2)));
  printf("s3 %d %d\n", sign(strcmp("abc", "abcd")), sign(strcmp("abcd", "abc")));

  // IEEE-exact math: fabs/sqrt/floor/ceil are exact (sqrt correctly
  // rounded), so %f output is bit-identical.
  printf("f1 %f %f %f\n", fabs(-2.5), fabs(2.5), fabs(0.0));
  printf("f2 %f %f %f\n", sqrt(2.0), sqrt(0.0), sqrt(144.0));
  printf("f3 %f %f %f\n", floor(1.7), floor(-1.5), floor(2.0));
  printf("f4 %f %f %f\n", ceil(1.2), ceil(-1.5), ceil(-3.0));

  // exit with a nonzero status: the rc= lines must match.
  exit(7);
}
