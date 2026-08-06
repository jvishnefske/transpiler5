// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// FR-65 rejection (pointer arithmetic): the direct-indexing Vec arm covers only
// `a[i]` and `free`. A byte-arithmetic advance `a++` on the buffer is NOT a
// subscript, so the usage accounting leaves `a` UNLIFTED and it keeps its
// historical located rejection of a non-constant-size heap allocation. Narrow
// by design: pointer arithmetic / a cursor model is a documented follow-on.
#include <stdlib.h>

void f(unsigned int n) {
  int *a = malloc(n * sizeof(int));
  a[0] = 1;
  a++; // pointer arithmetic: not a[i]/free, disqualifies the Vec lift
  free(a);
}
// CHECK: error: unsupported: allocation size is not a compile-time constant
