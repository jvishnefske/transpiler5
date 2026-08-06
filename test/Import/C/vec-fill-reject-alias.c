// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// FR-65 rejection (alias): a pointer copy `int *p = a` creates a second name for
// the buffer. The Vec arm requires every reference to `a` to be a subscript base
// or a `free` argument; a copy `p = a` is neither, so `a` stays UNLIFTED and
// keeps its historical located non-constant-size rejection.
#include <stdlib.h>

void f(unsigned int n) {
  int *a = malloc(n * sizeof(int));
  int *p = a; // alias: a second owner name, disqualifies the Vec lift
  p[0] = 1;
  free(a);
}
// CHECK: error: unsupported: allocation size is not a compile-time constant
