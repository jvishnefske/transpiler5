// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/int-cast-real-base.c 2>&1 | FileCheck %s --check-prefix=INTCAST
// RUN: not emitrust-import-c %t/null-only-deref.c 2>&1 | FileCheck %s --check-prefix=NULLDEREF

// CTS-P9 boundaries of the null-ternary model: only a base-less
// (statically null) region folds its pointer-to-int cast to 0, and a
// base-less region still has nothing to dereference.

// `(int) p` of a pointer with a real base object would materialize an
// address value; it keeps its located rejection.
// INTCAST: int-cast-real-base.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported cast (PointerToIntegral)

//--- int-cast-real-base.c
int main(void) {
  int x = 1;
  int *p = &x;
  return (int) p;
}

// A pointer whose region only ever unites null constants (here through
// a ternary) has no base object; dereferencing it stays rejected.
// NULLDEREF: null-only-deref.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: dereference of a pointer that is only ever null

//--- null-only-deref.c
int main(void) {
  int i = 1;
  int *q = 0;
  q = i ? 0 : q;
  return *q;
}
