// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/long-double-var.c 2>&1 | FileCheck %s --check-prefix=LDVAR
// RUN: not emitrust-import-c %t/long-double-literal.c 2>&1 | FileCheck %s --check-prefix=LDLIT
// RUN: not emitrust-import-c %t/long-double-hex.c 2>&1 | FileCheck %s --check-prefix=LDHEX

// C99-29 boundary: long double stays outside the type policy (Rust has
// no portable f80/f128 counterpart), so every literal spelling that types
// as long double — decimal L suffix or hexadecimal L suffix — keeps the
// located builtin-type rejection, as does the variable type itself.

//--- long-double-var.c
int main(void) {
  long double x = 1.5;
  return (int)x;
}
// LDVAR: long-double-var.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported builtin type 'long double'

// The L-suffixed literal is long double even when the target is double:
// the literal's own type is rejected before the narrowing cast.
//--- long-double-literal.c
int main(void) {
  double d = 1.5L;
  return (int)d;
}
// LDLIT: long-double-literal.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported builtin type 'long double'

// A hexadecimal long double literal is rejected the same way.
//--- long-double-hex.c
int main(void) {
  double d = 0x1.8p3L;
  return (int)d;
}
// LDHEX: long-double-hex.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported builtin type 'long double'
