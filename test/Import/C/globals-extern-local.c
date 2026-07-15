// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// C99-14 boundary: block-scope extern declarations are rejected; the
// file-scope declaration must be used instead.
int g;

int f(void) {
  extern int g;
  return g;
}

int main(void) {
  return f();
}

// CHECK: globals-extern-local.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: extern local variable
