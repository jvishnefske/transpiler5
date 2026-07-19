// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/star-width.c 2>&1 | FileCheck %s --check-prefix=STARW
// RUN: not emitrust-import-c %t/star-precision.c 2>&1 | FileCheck %s --check-prefix=STARP
// RUN: not emitrust-import-c %t/long-double.c 2>&1 | FileCheck %s --check-prefix=LONGDOUBLE
// RUN: not emitrust-import-c %t/size-t-length.c 2>&1 | FileCheck %s --check-prefix=SIZET
// RUN: not emitrust-import-c %t/pointer.c 2>&1 | FileCheck %s --check-prefix=POINTER
// RUN: not emitrust-import-c %t/count.c 2>&1 | FileCheck %s --check-prefix=COUNT
// RUN: not emitrust-import-c %t/hex-float.c 2>&1 | FileCheck %s --check-prefix=HEXFLOAT
// RUN: not emitrust-import-c %t/alt-on-d.c 2>&1 | FileCheck %s --check-prefix=ALTD
// RUN: not emitrust-import-c %t/plus-on-u.c 2>&1 | FileCheck %s --check-prefix=PLUSU
// RUN: not emitrust-import-c %t/zero-on-c.c 2>&1 | FileCheck %s --check-prefix=ZEROC
// RUN: not emitrust-import-c %t/zero-on-s.c 2>&1 | FileCheck %s --check-prefix=ZEROS
// RUN: not emitrust-import-c %t/precision-on-c.c 2>&1 | FileCheck %s --check-prefix=PRECC
// RUN: not emitrust-import-c %t/wide-char.c 2>&1 | FileCheck %s --check-prefix=WIDEC
// RUN: not emitrust-import-c %t/short-float.c 2>&1 | FileCheck %s --check-prefix=SHORTF
// RUN: not emitrust-import-c %t/long-long-float.c 2>&1 | FileCheck %s --check-prefix=LLF
// RUN: not emitrust-import-c %t/huge-width.c 2>&1 | FileCheck %s --check-prefix=HUGEW
// RUN: not emitrust-import-c %t/float-to-x.c 2>&1 | FileCheck %s --check-prefix=FLOATX

// C99-47 boundaries: directives outside the supported
// `%[flags][width][.precision][length]conv` grammar keep located
// rejections.

// A '*' field width consumes a runtime argument and stays rejected.
//--- star-width.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%*d\n", 5, 42);
  return 0;
}
// STARW: star-width.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: '*' field width in printf format

// A '*' precision consumes a runtime argument and stays rejected.
//--- star-precision.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%.*f\n", 3, 1.5);
  return 0;
}
// STARP: star-precision.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: '*' precision in printf format

// The 'L' (long double) length modifier stays rejected: long double has
// no Rust representation in the subset.
//--- long-double.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%Lf\n", 1.5L);
  return 0;
}
// LONGDOUBLE: long-double.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported printf length modifier 'L'

// The 'z' (size_t) length modifier stays rejected (as do 'j' and 't').
//--- size-t-length.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%zu\n", sizeof(int));
  return 0;
}
// SIZET: size-t-length.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported printf length modifier 'z'

// %p stays rejected by design: pointer provenance is compiled away by the
// pointer decomposition, so no address exists to print.
//--- pointer.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%p\n", (void *)0);
  return 0;
}
// POINTER: pointer.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported printf format specifier '%p'

// %n writes through a pointer argument and stays rejected.
//--- count.c
int printf(const char *fmt, ...);
int main(void) {
  int n;
  printf("abc%n\n", &n);
  return 0;
}
// COUNT: count.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported printf format specifier '%n'

// %a/%A (hex float) stays rejected.
//--- hex-float.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%a\n", 1.5);
  return 0;
}
// HEXFLOAT: hex-float.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported printf format specifier '%a'

// '#' is undefined for d/i/u/c/s (C99 7.19.6.1p6) and is rejected rather
// than silently dropped.
//--- alt-on-d.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%#d\n", 42);
  return 0;
}
// ALTD: alt-on-d.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: '#' flag on printf '%d'

// '+' and ' ' are defined only for the signed and floating conversions.
//--- plus-on-u.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%+u\n", 42u);
  return 0;
}
// PLUSU: plus-on-u.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: '+' or ' ' flag on printf '%u'

// '0' is defined only for the numeric conversions; %0c is undefined.
//--- zero-on-c.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%05c\n", 65);
  return 0;
}
// ZEROC: zero-on-c.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: '0' flag on printf '%c'

// '0' on %s is undefined as well.
//--- zero-on-s.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%010s\n", "x");
  return 0;
}
// ZEROS: zero-on-s.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: '0' flag on printf '%s'

// Precision on %c is undefined.
//--- precision-on-c.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%.3c\n", 65);
  return 0;
}
// PRECC: precision-on-c.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: precision on printf '%c'

// %lc/%ls are the wide-character conversions and stay rejected.
//--- wide-char.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%lc\n", 65);
  return 0;
}
// WIDEC: wide-char.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: length modifier on printf '%c'

// h/hh apply only to the integer conversions.
//--- short-float.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%hf\n", 1.5);
  return 0;
}
// SHORTF: short-float.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: length modifier 'h' on printf '%f'

// ll does not apply to the floating conversions (undefined in C99).
//--- long-long-float.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%lle\n", 1.5);
  return 0;
}
// LLF: long-long-float.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: length modifier 'll' on printf '%e'

// A field width that cannot fit in an i32 is rejected up front.
//--- huge-width.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%9999999999d\n", 1);
  return 0;
}
// HUGEW: huge-width.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: printf field width too large

// A non-integer argument to an integer conversion keeps the mismatch
// rejection.
//--- float-to-x.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%x\n", 1.5);
  return 0;
}
// FLOATX: float-to-x.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: printf argument 1 does not match its format specifier
