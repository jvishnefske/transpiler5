// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/va-copy.c 2>&1 | FileCheck %s --check-prefix=VACOPY
// RUN: not emitrust-import-c %t/ap-escape.c 2>&1 | FileCheck %s --check-prefix=ESCAPE
// RUN: not emitrust-import-c %t/addr-of.c 2>&1 | FileCheck %s --check-prefix=ADDROF

// Boundaries of va_list monomorphization (varargs-monomorph.c). The
// bounded shape is: ap never escapes the variadic definition, no
// va_copy, and every call to the variadic is a direct call (so the
// clone set is enumerable). Each violation is a located rejection with
// its own wording; defs whose bodies use va_list in other out-of-scope
// ways keep the blanket "variadic function definition" rejection
// (varargs-def-invalid.c).

// va_copy duplicates the consumption cursor into a second va_list
// object; the cloned-cursor lifetime rules are out of scope.
//--- va-copy.c
void vc(int n, ...) {
  __builtin_va_list ap, aq;
  __builtin_va_start(ap, n);
  __builtin_va_copy(aq, ap);
  int v = __builtin_va_arg(aq, int);
  n = v;
  __builtin_va_end(aq);
  __builtin_va_end(ap);
}
int main(void) {
  vc(1, 2);
  return 0;
}
// VACOPY: va-copy.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: va_copy

// ap passed to another function escapes the definition: the callee
// would consume varargs the monomorphizer cannot see.
//--- ap-escape.c
int vhelp(__builtin_va_list ap);
void vd(int n, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, n);
  vhelp(ap);
  __builtin_va_end(ap);
}
int main(void) {
  vd(1, 2);
  return 0;
}
// ESCAPE: ap-escape.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: va_list escapes variadic definition

// Taking the address of a va_list-using variadic definition makes its
// call sites non-enumerable (indirect calls could reach it), so the
// address-of is rejected even when direct calls also exist.
//--- addr-of.c
void va(int n, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, n);
  int v = __builtin_va_arg(ap, int);
  n = v;
  __builtin_va_end(ap);
}
int main(void) {
  void *h = (void *)&va;
  va(1, 2);
  return h == 0;
}
// ADDROF: addr-of.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: address of variadic definition
