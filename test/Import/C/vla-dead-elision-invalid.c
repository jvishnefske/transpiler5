// CTS-F (00207) negative space: dead-VLA elision covers ONLY an
// unreferenced VLA whose size expression is side-effect-free. A VLA that
// IS referenced keeps the verbatim rejection, and a VLA whose size
// expression has side effects stays rejected even when the object is
// unused (dropping it would silently lose the call). Both pins reuse the
// existing wording at the declared variable's location.
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/referenced.c 2>&1 | FileCheck %s --check-prefix=REFERENCED
// RUN: not emitrust-import-c %t/side-effect.c 2>&1 | FileCheck %s --check-prefix=SIDEEFFECT

//--- referenced.c
// The array object is used, so the VLA cannot be elided.
void f(int n) {
  char test[n];
  test[0] = 1;
}
// REFERENCED: referenced.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-constant array size

//--- side-effect.c
// The object is dead, but the size expression calls a function: eliding
// the declaration would drop the call, so the rejection stands.
int f(void);
void g(void) {
  char t[f()];
}
// SIDEEFFECT: side-effect.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-constant array size
