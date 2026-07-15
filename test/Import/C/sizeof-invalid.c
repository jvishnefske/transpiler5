// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

long vla_size(int n) {
  // sizeof of a variable-length array is the one sizeof C evaluates at
  // run time; there is no constant to fold, so it is rejected.
  return sizeof(int[n]);
}

// CHECK: sizeof-invalid.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: sizeof/alignof of a variable-length array
