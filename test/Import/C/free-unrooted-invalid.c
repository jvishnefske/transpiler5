// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// W4.2e Part A: `free` of a pointer not rooted in a recognized local
// allocation has no modeled deallocation and is rejected located (a defined
// program only frees what it allocated; the model tracks only its own
// synthesized backings).
#include <stdlib.h>
int g;
void f(void) {
  free(&g);
}
// CHECK: error: unsupported: free of a pointer not rooted in a recognized allocation
