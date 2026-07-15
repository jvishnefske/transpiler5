// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// C99-14: aggregate initializer lists for globals are deferred (C99-11);
// zero-initialized aggregate globals are supported.
int arr[2] = {1, 2};

int main(void) {
  return arr[0];
}

// CHECK: globals-aggregate-init.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aggregate initializer for a global variable
