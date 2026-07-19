// REQUIRES: cargo
// C99-28 completion: differential regression test for string literals in
// expression positions and the full escape set. Covers the simple escapes
// (\a \b \f \v \r \?) plus octal and hex forms in both the char-array
// initializer and the literal-region (char * binding) paths, unsigned
// char array initializers at block and file scope, direct literal
// subscripts ("abc"[i], constant and variable index), the
// deref-of-arithmetic spelling *("qr" + 1), sizeof of a literal
// (including an adjacent-literal concatenation), concatenation filling a
// char array with zero fill past the joined bytes, and a literal passed
// straight to a user-defined function's char * parameter. Byte-identical
// stdout and exit codes against the clang-built native binary are
// required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name strings_exprs_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/strings_exprs_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

unsigned char ug[] = "hi";

int first(char *s) {
  return s[0];
}

int main(void) {
  char e[12] = "a\a\b\f\v\r\?\x41\101z";
  for (int i = 0; i < 12; i++)
    printf("%d ", e[i]);
  printf("\n");

  char *p = "q\a\b\f\v\?\x42\102";
  while (*p) {
    printf("%d ", *p);
    p++;
  }
  printf("\n");

  unsigned char u[4] = "ab";
  unsigned char v[] = "cd";
  printf("%d %d %d %d %d %d\n", u[0], u[3], v[1], v[2], ug[0], ug[2]);

  int i = 2;
  printf("%d %d %d\n", "abc"[i], "xy"[0], *("qr" + 1));

  printf("%d %d\n", (int)sizeof("abc"), (int)sizeof("ab" "cd"));

  char cat[8] = "ab" "cd";
  printf("%d %d %d\n", cat[3], cat[4], cat[7]);

  printf("%d\n", first("hello"));
  return 0;
}
