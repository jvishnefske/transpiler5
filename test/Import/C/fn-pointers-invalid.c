// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/variadic.c 2>&1 | FileCheck %s --check-prefix=VARIADIC
// RUN: not emitrust-import-c %t/mismatch.c 2>&1 | FileCheck %s --check-prefix=MISMATCH
// RUN: not emitrust-import-c %t/cast-mismatch.c 2>&1 | FileCheck %s --check-prefix=CASTMISMATCH
// RUN: not emitrust-import-c %t/noproto-args.c 2>&1 | FileCheck %s --check-prefix=NOPROTO
// RUN: not emitrust-import-c %t/pointer-component.c 2>&1 | FileCheck %s --check-prefix=COMPONENT

// FR-29: every unsupported function pointer construct fails the import
// with a located diagnostic: a pointer to a variadic function, a function
// bound to a pointer with a different signature (including a
// prototype-less `int (*)()` pointer bound to a function with
// parameters), a call with arguments through a prototype-less pointer,
// and a fn_ptr whose component types fall outside the supported set.

//--- variadic.c
int printf(const char *, ...);
int main(void) {
  int (*fp)(const char *, ...) = printf;
  return 0;
}
// VARIADIC: error: unsupported: variadic function pointer type

//--- mismatch.c
int add(int a, int b) { return a + b; }
int main(void) {
  int (*np)() = add;
  return np();
}
// MISMATCH: error: unsupported: function 'add' does not match the function pointer signature

//--- cast-mismatch.c
int add(int a, int b) { return a + b; }
int main(void) {
  int (*fp)(int) = (int (*)(int))add;
  return fp(1);
}
// CASTMISMATCH: error: unsupported: function 'add' does not match the function pointer signature

//--- noproto-args.c
// Callsite-prototype inference (00209 wave) derives (int, int) -> int
// from the call, so the failure is now the incompatible BINDING of
// `zero` at the initializer, not the call itself. The blanket
// no-prototype-call rejection survives for non-decl-traceable callees
// (pinned in fnptr-noproto-infer-invalid.c's member case).
int zero(void) { return 0; }
int main(void) {
  int (*np)() = zero;
  return np(1, 2);
}
// NOPROTO: error: unsupported: function 'zero' does not match the function pointer signature

//--- pointer-component.c
void writer(int *out) { *out = 1; }
int main(void) {
  void (*fp)(int *) = writer;
  return 0;
}
// COMPONENT: error: unsupported: pointer type outside a parameter position
