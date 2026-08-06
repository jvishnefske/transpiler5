// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// FR-65 rejection (function-passing): passing the buffer `a` to another function
// `g(a)` shares/escapes it. The Vec arm accounts only for `a[i]` subscripts and
// `free(a)`; a call argument is neither, so `a` stays UNLIFTED and keeps its
// historical located non-constant-size rejection. Narrow by design: a
// Vec->`&mut [T]` span bridge for function-passing is a documented follow-on.
#include <stdlib.h>
void g(int *);

void f(unsigned int n) {
  int *a = malloc(n * sizeof(int));
  a[0] = 1;
  g(a); // passed to a function: disqualifies the Vec lift
  free(a);
}
// CHECK: error: unsupported: allocation size is not a compile-time constant
