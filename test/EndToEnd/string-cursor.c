// REQUIRES: cargo
// CTS-P1: differential regression test for `char *` bound to string
// literals. A literal-bound pointer is a cursor into a read-only region
// backed by an immutable byte array (bytes plus terminating NUL): walking
// with ++ terminates on the NUL, subscript and index arithmetic read in
// both directions, %s prints from the cursor (including mid-literal), a
// definition-less strlen counts to the NUL from any cursor position,
// copies of the pointer share the region, a strcpy-style loop copies the
// region into a writable char array, and a literal compared against a
// null pointer constant folds to "not equal". Byte-identical stdout and
// exit codes against the clang-built native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name string_cursor_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/string_cursor_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);
int strlen(char *);

int main(void) {
  char *p = "hello";

  // Walk the literal, printing each byte and its value; the walk ends on
  // the terminating NUL that the region's backing must include.
  char *b;
  for (b = p; *b != 0; b++)
    printf("%c: %d\n", *b, *b);

  // Index arithmetic in both directions relative to a moved cursor.
  char *mid = p + 3;
  printf("mid %c back %c fwd %c\n", *mid, mid[-2], mid[1]);
  printf("dist %d\n", (int)(mid - p));

  // %s from cursor 0 and from a mid-literal cursor.
  printf("s0 %s|\n", p);
  printf("s3 %s|\n", p + 3);

  // Adjacent literals concatenate into one region; subscript reads every
  // byte including the NUL.
  char *s = "abc" "def";
  printf("cat %c%c%c%c%c%c %d\n", s[0], s[1], s[2], s[3], s[4], s[5], s[6]);

  // strlen from several cursor positions.
  printf("len %d %d %d\n", strlen(p), strlen(p + 2), strlen(s));

  // Copies share the region; the postfix walk leaves the copy on the NUL.
  char *src = p;
  int n = 0;
  while (*src != 0) {
    src++;
    n++;
  }
  printf("walked %d\n", n);

  // strcpy-style copy through a writable array cursor (reads through the
  // read-only region, writes through the array region).
  char dest[10];
  char *d = &dest[0];
  char *from = p;
  while (*from != 0)
    *d++ = *from++;
  *d = 0;
  printf("copied %s|\n", dest);

  // A literal is never null in C; both directions of the comparison fold.
  if ("abc" == (void *)0)
    printf("eq-null\n");
  if ((void *)0 != "xyz")
    printf("ne-null\n");

  return strlen(p) - 5;
}
