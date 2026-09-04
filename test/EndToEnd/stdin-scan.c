// REQUIRES: cargo
// FR-183: differential regression for standard input -- the `scanf`/`fscanf`
// reader over `stdin`, plus `fgets`/`getchar` INTERLEAVED on that same one
// stream. The emitted crate models C's `stdin` as a stateless
// `__EmitrustFile::Stdin` handle over a process-wide single-character
// pushback slot; nothing about that is observable except through the bytes
// each reader consumes, so the only honest oracle is stdout byte-identical
// to the clang-built native on the SAME input file. The entropy is the input
// itself, which no constant folding on either side can see.
//
// PORTABILITY, and it decides what is IN this file. Every semantic FR-183
// measured was measured on glibc/x86-64, and two of them -- a failed `%d`
// leaving a consumed sign behind, and `strtol` saturate-then-truncate on
// overflow -- are plausibly libc-specific while THIS oracle compares against
// the HOST libc. A differential encoding a glibc quirk would fail on Darwin,
// so the vectors below stay inside what C11 7.21.6.2 actually defines:
// matching input, a matching failure whose single offending character C
// guarantees is left unread, and end-of-file. The glibc-only edges are
// pinned in prose on the helpers and at Import level, never here.
//
// The five vectors cover, between them: `%d`, `%u`, `%c` (which does NOT
// skip leading whitespace -- vector 1 reads the newline `%u` stopped on),
// `%d %d` with an explicit whitespace directive, five SEQUENTIAL scanf
// calls threading one stream position, `fscanf(stdin, ...)` as the same
// reader, `fgets` picking up exactly where scanf stopped, `getchar` between
// them, a matching failure (return 0, variable untouched, offending byte
// still there for the next reader), and reads past end of input (-1).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name stdin_scan_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: printf '42 7 100 9\nhello world\nZ13\n' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/stdin_scan_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: printf '' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/stdin_scan_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: printf 'abc\n' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/stdin_scan_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: printf '5 x\n' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/stdin_scan_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: printf '  1\t2\n3 4 5\n' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/stdin_scan_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdio.h>

int main(void) {
  int a = -7;
  int b = -7;
  unsigned int u = 7u;
  char c = '?';
  char line[32];
  int ch;
  int r;

  /* A single %d: the leading-whitespace skip and the digit run. */
  r = scanf("%d", &a);
  printf("r1=%d a=%d\n", r, a);

  /* Two conversions with an explicit whitespace directive between them. */
  r = scanf("%d %d", &a, &b);
  printf("r2=%d a=%d b=%d\n", r, a, b);

  /* %u over a non-negative literal (a negative one is strtoul's
     negate-mod-2^64, which this portable differential deliberately omits). */
  r = scanf("%u", &u);
  printf("r3=%d u=%u\n", r, u);

  /* %c does NOT skip leading whitespace: it reads whatever byte the
     previous conversion stopped on. */
  r = scanf("%c", &c);
  printf("r4=%d c=%d\n", r, c);

  /* fgets on the SAME stream continues from the scanf position. */
  if (fgets(line, 32, stdin) != NULL)
    printf("g=[%s]\n", line);
  else
    printf("g=NULL\n");

  /* getchar interleaved between the two readers. */
  ch = getchar();
  printf("ch=%d\n", ch);

  /* fscanf(stdin, ...) is the same reader on the same position. */
  r = fscanf(stdin, "%d", &a);
  printf("r5=%d a=%d\n", r, a);

  /* A read past the end of input: -1, and the variable is untouched. */
  r = scanf("%d", &b);
  printf("r6=%d b=%d\n", r, b);
  return 0;
}
