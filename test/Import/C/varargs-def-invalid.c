// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/side-effect.c 2>&1 | FileCheck %s --check-prefix=SIDEEFFECT
// RUN: not emitrust-import-c %t/valist-local.c 2>&1 | FileCheck %s --check-prefix=VALOCAL
// RUN: not emitrust-import-c %t/valist-param.c 2>&1 | FileCheck %s --check-prefix=VAPARAM
// RUN: not emitrust-import-c %t/vprintf-call.c 2>&1 | FileCheck %s --check-prefix=VPRINTF

// CTS-P9 boundaries: va_list-free variadic definitions import as their
// fixed prototype, and call sites may only drop effect-free extras.
// CTS 00204 REVISION: a definition whose body uses va_list in the
// bounded monomorphizable shape (ap never escapes, no va_copy, every
// call direct) is no longer rejected — it clones per call site
// (varargs-monomorph.c); the out-of-scope va_list body shapes carry
// their own located rejections (varargs-monomorph-invalid.c), and the
// blanket "variadic function definition" rejection remains for any
// other va_list-using body.
// C99-37 rejection: the va_list type itself has no Rust representation
// outside a variadic definition — Rust has no stable varargs — so a
// va_list object elsewhere (local, parameter) is a located type
// rejection rather than a silent import of the target's
// register-save-area struct.

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
