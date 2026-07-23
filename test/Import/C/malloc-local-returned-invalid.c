// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// W4.2e Part A: a pointer into a callee-local heap allocation cannot be
// returned — the synthesized backing drops at scope end, so the returned
// cursor would dangle. The pointer-return classifier rejects it located
// (unchanged by Part A).
#include <stdlib.h>
int *f(void) {
  int *p = malloc(4 * sizeof(int));
  p[0] = 1;
  return p;
}
// CHECK: error: unsupported: returned pointer value
