// REQUIRES: cargo
// C99-28/47/48: differential regression test for the minimal string model.
// `char s[] = "..."` at block and file scope (sized and unsized, with
// zero fill past the literal), %s of string literals (escapes, braces,
// backslashes) and of char arrays honoring C's stop-at-first-NUL: an
// embedded NUL is written mid-array and the same array is printed before
// and after truncation, including an array whose first byte is NUL and a
// full array with no NUL before the writeback. puts covers the literal
// and char-array shapes (brace/quote escaping included), putchar covers
// boundary bytes and int-promoted chars. Byte-identical stdout and exit
// codes against the clang-built native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name strings_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/strings_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);
int puts(const char *);
int putchar(int);

char fileScope[] = "file scope";
char filePadded[8] = "pad";

int main(void) {
  // Sized local: bytes 'a','b',NUL assigned, 4th element default zero.
  char buf[4] = "ab";
  printf("%s|\n", buf);
  // Unsized local: length inferred as 6 (5 + NUL).
  char hello[] = "hi you";
  printf("%s|\n", hello);
  // Embedded NUL truncation: C stops printing at the first NUL.
  hello[2] = 0;
  printf("%s|\n", hello);
  // First byte NUL: prints as the empty string.
  buf[0] = 0;
  printf("%s|\n", buf);
  // Refill every byte, then restore a terminator mid-way.
  buf[0] = 'x';
  buf[1] = 'y';
  buf[2] = 'z';
  buf[3] = 0;
  printf("%s|\n", buf);
  // File scope, sized and unsized, plus zero-filled padding.
  printf("%s|%s|\n", fileScope, filePadded);
  // %s literals with every supported escape shape.
  printf("%s %s\n", "lit-one", "esc \"quoted\" back\\slash {brace}");
  puts("puts literal with {brace} and \"quote\"");
  puts(buf);
  puts(fileScope);
  putchar('A');
  putchar(66);
  putchar(32);
  putchar(126);
  putchar(321); /* converts to unsigned char 65 like C */
  char cv = 'q';
  putchar(cv);
  putchar('\n');
  return 0;
}
