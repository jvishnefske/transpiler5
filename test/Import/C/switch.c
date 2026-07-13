// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

int classify(int x) {
  switch (x) {
  case 0:
    return 1;
  default:
    return 2;
  }
}

// The diagnostic carries the file:line:col location of the switch.
// CHECK: switch.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: switch statement
