// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

union Value {
  int i;
  float f;
};

int main(void) {
  return 0;
}

// Unions are rejected with a located diagnostic.
// CHECK: union.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union type
