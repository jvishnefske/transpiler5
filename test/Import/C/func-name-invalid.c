// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/subscript.c 2>&1 | FileCheck %s --check-prefix=SUBSCRIPT
// RUN: not emitrust-import-c %t/value-use.c 2>&1 | FileCheck %s --check-prefix=VALUE
// RUN: not emitrust-import-c %t/format.c 2>&1 | FileCheck %s --check-prefix=FORMAT

// C99-29 boundary: `__func__` is modeled only in the string-literal
// positions (printf/puts '%s' arguments, char-pointer bindings, and
// string-helper arguments); anything that needs the name array as a
// first-class place or value keeps a located rejection.

// Element access reads the name array as a place; outside the modeled
// positions the identifier is rejected by name.
//--- subscript.c
int main(void) {
  return __func__[0];
}
// SUBSCRIPT: subscript.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported use of '__func__' outside a string literal position

// __FUNCTION__ in a plain value position (no modeled consumer) is
// rejected under its own spelling.
//--- value-use.c
int take(int);
int main(void) {
  return take((int)__FUNCTION__[0]);
}
// VALUE: value-use.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported use of '__FUNCTION__' outside a string literal position

// The printf format position requires a spelled ordinary string literal:
// the format grammar is parsed at import time, and a computed format —
// even the constant __func__ — keeps the historical format rejection.
//--- format.c
int printf(const char *, ...);
int main(void) {
  printf(__func__);
  return 0;
}
// FORMAT: format.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: printf format must be an ordinary string literal
