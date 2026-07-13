// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

int sum(int count, ...) {
  return count;
}

// Variadic function definitions are rejected with a located diagnostic.
// CHECK: varargs-def.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: variadic function definition
