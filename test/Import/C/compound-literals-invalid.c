// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/scalar.c 2>&1 | FileCheck %s --check-prefix=SCALAR
// RUN: not emitrust-import-c %t/global-bind.c 2>&1 | FileCheck %s --check-prefix=GLOBALBIND
// RUN: not emitrust-import-c %t/return-escape.c 2>&1 | FileCheck %s --check-prefix=RETURN
// RUN: not emitrust-import-c %t/static-local.c 2>&1 | FileCheck %s --check-prefix=STATICLOCAL

// C99-13 boundaries: each file below exercises one located rejection
// around compound literals in expression position.

// A scalar compound literal has no aggregate-init lowering (the C99-13
// subset covers struct/union/array literals).
//--- scalar.c
int main(void) { return (int){5}; }
// SCALAR: scalar.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: compound literal of non-aggregate type

// A global pointer bound to a block-scope compound literal is a borrow
// escaping the literal's block — the temp dies at the block's end.
//--- global-bind.c
int *g;
void bind(void) { g = (int[]){1, 2}; }
int main(void) {
  bind();
  return g[0];
}
// GLOBALBIND: global-bind.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global pointer bound to local object 'compound literal' (the borrow would outlive the object)

// Returning a pointer into the literal would dangle (the temp is
// callee-local); the historical pointer-return rejection fires.
//--- return-escape.c
int *escape(void) { return (int[]){1, 2, 3}; }
int main(void) { return escape()[0]; }
// RETURN: return-escape.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: returned pointer value

// A block-scope static pointer cannot hold the literal's automatic
// address; clang's own constant-initializer check rejects before import.
//--- static-local.c
int main(void) {
  static int *p = (int[]){1, 2};
  return p[0];
}
// STATICLOCAL: static-local.c:{{[0-9]+}}:{{[0-9]+}}: error: initializer element is not a compile-time constant
