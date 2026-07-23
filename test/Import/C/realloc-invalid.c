// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// W4.2e Part A: realloc has no representation in the fixed-backing model
// (the synthesized backing array cannot resize), so it is rejected located.
#include <stdlib.h>
int f(void) {
  int *p = malloc(4 * sizeof(int));
  p[0] = 1;
  p = realloc(p, 8 * sizeof(int));
  return p[0];
}
// CHECK: error: unsupported: realloc is not part of the supported allocation model
