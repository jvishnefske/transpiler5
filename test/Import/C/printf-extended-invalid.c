// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/precision.c 2>&1 | FileCheck %s --check-prefix=PRECISION
// RUN: not emitrust-import-c %t/long-long.c 2>&1 | FileCheck %s --check-prefix=LONGLONG
// RUN: not emitrust-import-c %t/short.c 2>&1 | FileCheck %s --check-prefix=SHORT
// RUN: not emitrust-import-c %t/pointer.c 2>&1 | FileCheck %s --check-prefix=POINTER
// RUN: not emitrust-import-c %t/width-on-c.c 2>&1 | FileCheck %s --check-prefix=WIDTHC
// RUN: not emitrust-import-c %t/width-on-s.c 2>&1 | FileCheck %s --check-prefix=WIDTHS
// RUN: not emitrust-import-c %t/width-on-f.c 2>&1 | FileCheck %s --check-prefix=WIDTHF
// RUN: not emitrust-import-c %t/float-to-x.c 2>&1 | FileCheck %s --check-prefix=FLOATX

// C99-47 boundaries: directives outside the supported
// `%[flags][width][length]conv` grammar keep located rejections.

// Precision is unsupported on every conversion (including %.3s and %.2f).
//--- precision.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%.3s\n", "abcdef");
  return 0;
}
// PRECISION: precision.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: precision in printf format specifier

// The 'll' length modifier (and long long arguments) stay rejected.
//--- long-long.c
int printf(const char *fmt, ...);
int main(void) {
  long long x = 1;
  printf("%llx\n", x);
  return 0;
}
// LONGLONG: long-long.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported printf length modifier 'll'

// The 'h' length modifier stays rejected.
//--- short.c
int printf(const char *fmt, ...);
int main(void) {
  short x = 1;
  printf("%hd\n", x);
  return 0;
}
// SHORT: short.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported printf length modifier 'h'

// %p has no Rust representation in the subset; the conversion itself is
// rejected (before its argument is imported).
//--- pointer.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%p\n", (void *)0);
  return 0;
}
// POINTER: pointer.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported printf format specifier '%p'

// Width and flags on %c are out of the supported grammar.
//--- width-on-c.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%5c\n", 65);
  return 0;
}
// WIDTHC: width-on-c.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: flags, width, or length on printf '%c'

// Width and flags on %s are out of the supported grammar.
//--- width-on-s.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%10s\n", "x");
  return 0;
}
// WIDTHS: width-on-s.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: flags, width, or length on printf '%s'

// Width and flags on %f would pad the helper's String, not the number,
// and are rejected.
//--- width-on-f.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%8f\n", 1.5);
  return 0;
}
// WIDTHF: width-on-f.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: flags or width on printf '%f'

// A non-integer argument to an integer conversion keeps the mismatch
// rejection.
//--- float-to-x.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%x\n", 1.5);
  return 0;
}
// FLOATX: float-to-x.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: printf argument 1 does not match its format specifier
