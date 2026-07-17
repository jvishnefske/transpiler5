// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/char-ptr-literal.c 2>&1 | FileCheck %s --check-prefix=CHARPTR
// RUN: not emitrust-import-c %t/nonascii-local.c 2>&1 | FileCheck %s --check-prefix=NONASCIILOCAL
// RUN: not emitrust-import-c %t/nonascii-global.c 2>&1 | FileCheck %s --check-prefix=NONASCIIGLOBAL
// RUN: not emitrust-import-c %t/nonascii-percent-s.c 2>&1 | FileCheck %s --check-prefix=NONASCIIS
// RUN: not emitrust-import-c %t/nul-percent-s.c 2>&1 | FileCheck %s --check-prefix=NULS
// RUN: not emitrust-import-c %t/percent-s-scalar.c 2>&1 | FileCheck %s --check-prefix=SCALARS
// RUN: not emitrust-import-c %t/puts-value.c 2>&1 | FileCheck %s --check-prefix=PUTSVALUE
// RUN: not emitrust-import-c %t/putchar-value.c 2>&1 | FileCheck %s --check-prefix=PUTCHARVALUE

// C99-47/28 boundaries: string shapes outside the supported subset keep
// located rejections.

// A `char *` bound to a string literal is supported (CTS-P1), but the
// literal's backing keeps the C99-28 ASCII policy: a non-ASCII byte in a
// literal bound to a pointer is rejected.
//--- char-ptr-literal.c
int main(void) {
  char *p = "caf\xff";
  return p[0];
}
// CHARPTR: char-ptr-literal.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-ASCII byte in string literal bound to a pointer

// Non-ASCII bytes in a block-scope string initializer are rejected so the
// array's contents stay exact through the ASCII-only printing helpers.
//--- nonascii-local.c
int main(void) {
  char s[8] = "caf\xff";
  return s[0];
}
// NONASCIILOCAL: nonascii-local.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-ASCII byte in string literal initializer

// The same rejection applies at file scope.
//--- nonascii-global.c
char g[] = "caf\xc3\xa9";
int main(void) { return g[0]; }
// NONASCIIGLOBAL: nonascii-global.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-ASCII byte in string literal initializer

// A %s literal argument with a non-ASCII byte would not survive rustc's
// UTF-8 check verbatim and is rejected.
//--- nonascii-percent-s.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%s\n", "a\xffz");
  return 0;
}
// NONASCIIS: nonascii-percent-s.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-printable or non-ASCII byte in printf '%s' string literal

// A %s literal argument with an embedded NUL would print more than C
// (which stops at the NUL) and is rejected.
//--- nul-percent-s.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%s\n", "a\0b");
  return 0;
}
// NULS: nul-percent-s.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: NUL byte in printf '%s' string literal

// A non-string argument to %s is rejected.
//--- percent-s-scalar.c
int printf(const char *fmt, ...);
int main(void) {
  int x = 1;
  printf("%s\n", (char *)&x);
  return 0;
}
// SCALARS: percent-s-scalar.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: printf '%s' argument must be a string literal or a char array

// puts/putchar are statement-position only; a value use of their int
// result keeps a located rejection.
//--- puts-value.c
int puts(const char *s);
int main(void) {
  int r = puts("x");
  return r;
}
// PUTSVALUE: puts-value.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: puts return value must be unused

//--- putchar-value.c
int putchar(int c);
int main(void) {
  return putchar(65);
}
// PUTCHARVALUE: putchar-value.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: putchar return value must be unused
