// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/nul.c 2>&1 | FileCheck %s --check-prefix=NUL
// RUN: not emitrust-import-c %t/nonascii.c 2>&1 | FileCheck %s --check-prefix=NONASCII

// C printf stops printing at an embedded NUL while Rust's print! would emit
// every byte, and a byte outside printable ASCII (plus \n \t \r) would pass
// into the generated Rust source verbatim and fail rustc's UTF-8 check, so
// both format strings are rejected with a located diagnostic instead of
// producing diverging or unbuildable output.

// NUL: nul.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: NUL byte in printf format
// NONASCII: nonascii.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-printable or non-ASCII byte in printf format

//--- nul.c
int printf(const char *fmt, ...);

int main(void) {
  printf("a\0b\n");
  return 0;
}

//--- nonascii.c
int printf(const char *fmt, ...);

int main(void) {
  printf("\xff\n");
  return 0;
}
