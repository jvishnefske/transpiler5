// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// W4.2e Part A: a local heap allocation must not escape the function that
// owns its backing. Storing the pointer into a global pointer is not a
// modeled source for that global (its value is a backing cursor, not a
// global region base), so the assignment is rejected located.
#include <stdlib.h>
int *g;
void f(void) {
  int *p = malloc(4 * sizeof(int));
  p[0] = 1;
  g = p;
}
// CHECK: error: unsupported: pointer assignment would rebind to a different object
