// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/valist-def.c 2>&1 | FileCheck %s --check-prefix=VALIST
// RUN: not emitrust-import-c %t/side-effect.c 2>&1 | FileCheck %s --check-prefix=SIDEEFFECT
// RUN: not emitrust-import-c %t/varargs-read.c 2>&1 | FileCheck %s --check-prefix=VAREAD
// RUN: not emitrust-import-c %t/valist-local.c 2>&1 | FileCheck %s --check-prefix=VALOCAL
// RUN: not emitrust-import-c %t/valist-param.c 2>&1 | FileCheck %s --check-prefix=VAPARAM
// RUN: not emitrust-import-c %t/vprintf-call.c 2>&1 | FileCheck %s --check-prefix=VPRINTF

// CTS-P9 boundaries: only va_list-free variadic definitions import as
// their fixed prototype, and call sites may only drop effect-free extras.
// C99-37 permanent rejection: the va_list type itself has no Rust
// representation in ANY position — Rust has no stable varargs — so a
// va_list object outside a variadic definition (local, parameter) is a
// located type rejection rather than a silent import of the target's
// register-save-area struct.

// A definition whose body touches va_list (va_start/va_arg) keeps today's
// located rejection at the definition.
//--- valist-def.c
int sum(int count, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, count);
  int x = __builtin_va_arg(ap, int);
  __builtin_va_end(ap);
  return x;
}
// VALIST: valist-def.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: variadic function definition

// A dropped extra argument with side effects would silently lose the
// effect, so the call is rejected with a located diagnostic.
//--- side-effect.c
int hits;

int f(int a, ...) {
  return a;
}

int bump(void) {
  hits = hits + 1;
  return hits;
}

int main(void) {
  return f(1, bump());
}
// SIDEEFFECT: side-effect.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: extra argument to a variadic call has side effects

// A call to a variadic whose body reads its varargs is still rejected:
// the va_list-using definition itself keeps the rejection, calls or not.
//--- varargs-read.c
int sum(int count, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, count);
  int total = __builtin_va_arg(ap, int);
  __builtin_va_end(ap);
  return total;
}

int main(void) {
  return sum(1, 41);
}
// VAREAD: varargs-read.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: variadic function definition

// A va_list local in a NON-variadic function (outside the reach of the
// variadic-definition rejection) is rejected at the declaration: the
// type has no Rust representation (C99-37).
//--- valist-local.c
int f(int x) {
  __builtin_va_list ap;
  return x;
}
// VALOCAL: valist-local.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: va_list type

// A va_list parameter (a hand-rolled vprintf-style helper) is rejected
// at the parameter's type, before any body is imported.
//--- valist-param.c
int helper(__builtin_va_list ap) {
  return 0;
}
// VAPARAM: valist-param.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: va_list type

// The v*printf family is unreachable by construction: every call needs a
// va_list argument, and the va_list object's declaration is rejected
// first. (A body-less vprintf itself is a skipped system-header
// declaration; a call reached without the decl rejection would keep the
// C99-39 system-header use rejection.)
//--- vprintf-call.c
#include <stdio.h>
int main(void) {
  __builtin_va_list ap;
  vprintf("%d\n", ap);
  return 0;
}
// VPRINTF: vprintf-call.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: va_list type
