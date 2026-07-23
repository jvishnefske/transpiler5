// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// W4.2e Part A: a local heap allocation whose size is not a compile-time
// constant (nor a foldable-local expression) has no fixed backing extent
// and is rejected located — the foldable-local resolver folds `cap`-style
// automatic locals, but a genuine runtime size (a parameter here) cannot
// fold.
#include <stdlib.h>
int f(int n) {
  int *p = malloc(n * sizeof(int));
  p[0] = 1;
  return p[0];
}
// CHECK: error: unsupported: allocation size is not a compile-time constant
