// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/nonliteral.c 2>&1 | FileCheck %s --check-prefix=NONLITERAL
// RUN: not emitrust-import-c %t/precision.c 2>&1 | FileCheck %s --check-prefix=PRECISION
// RUN: not emitrust-import-c %t/literal-dest.c 2>&1 | FileCheck %s --check-prefix=LITDEST

// CTS-P9 boundaries: sprintf shapes outside the literal-format subset
// keep located rejections.

// The format must be an ordinary string literal, exactly like printf's.
//--- nonliteral.c
int sprintf(char *s, const char *fmt, ...);
int main(void) {
  char buf[8];
  const char *fmt = "%d";
  sprintf(buf, fmt, 1);
  return 0;
}
// NONLITERAL: nonliteral.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: sprintf format must be an ordinary string literal

// Precision stays outside the shared C99-47 directive grammar, so the
// shared translator keeps its located rejection for sprintf too.
//--- precision.c
int sprintf(char *s, const char *fmt, ...);
int main(void) {
  char buf[8];
  sprintf(buf, "%.3d", 5);
  return 0;
}
// PRECISION: precision.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: precision in printf format specifier

// A string-literal-backed region is read-only and cannot be the sprintf
// destination (same policy as the <string.h> copy helpers).
//--- literal-dest.c
int sprintf(char *s, const char *fmt, ...);
int main(void) {
  char *p = "abcdef";
  sprintf(p, "%d", 1);
  return 0;
}
// LITDEST: literal-dest.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a string literal region cannot be a mutable string argument
