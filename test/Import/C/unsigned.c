// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

int f(void) {
  unsigned int u = 1;
  return 0;
}

// Any unsigned type is rejected with a located diagnostic.
// CHECK: unsigned.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: unsigned integer type
