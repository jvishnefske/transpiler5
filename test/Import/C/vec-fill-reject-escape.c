// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// FR-65 rejection (escape / return): a `return a` hands the buffer's ownership
// to the caller. The Vec arm is single-owner (the `Vec` drops at scope end), so
// an escaping buffer is out of scope — `a` stays UNLIFTED and keeps its located
// pointer-return rejection. Narrow by design: a Vec->`&mut [T]` span bridge for
// function-passing/return is a documented follow-on.
#include <stdlib.h>

int *f(unsigned int n) {
  int *a = malloc(n * sizeof(int));
  a[0] = 1;
  return a; // escape: the buffer leaves the function
}
// CHECK: error: unsupported: returned pointer value
