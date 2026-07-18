// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/valist-def.c 2>&1 | FileCheck %s --check-prefix=VALIST
// RUN: not emitrust-import-c %t/side-effect.c 2>&1 | FileCheck %s --check-prefix=SIDEEFFECT
// RUN: not emitrust-import-c %t/varargs-read.c 2>&1 | FileCheck %s --check-prefix=VAREAD

// CTS-P9 boundaries: only va_list-free variadic definitions import as
// their fixed prototype, and call sites may only drop effect-free extras.

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
