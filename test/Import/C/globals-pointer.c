// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// C99-14 boundary: pointer-typed globals need the (future) pointer model.
int x;
int *p;

int main(void) {
  return x;
}

// CHECK: globals-pointer.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-typed global variable
