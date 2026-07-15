// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// C99-14: taking the address of a global is rejected (a pointer into a
// global would dangle from the staged local copy the access model uses).
int g;

void take(int *p) {
  *p = 1;
}

int main(void) {
  take(&g);
  return 0;
}

// CHECK: globals-invalid.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: taking the address of a global variable
