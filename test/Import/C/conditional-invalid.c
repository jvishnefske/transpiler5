// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

struct Pair {
  int a;
  int b;
};

struct Pair choose(int c, struct Pair x, struct Pair y) {
  return c ? x : y;
}

// Only scalar (integer and float) conditional results are supported; the
// rejection carries the operator's location.
// CHECK: conditional-invalid.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conditional operator on a non-scalar operand
