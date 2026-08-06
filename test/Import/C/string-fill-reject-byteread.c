// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// FR-64 rejection (byte read): a `char *` buffer that is READ per byte
// (`x = a[0]`) is NOT the constant-fill string idiom — its value is observed,
// not just its `puts`/`Display` bytes — so it is NOT lifted to `String` and
// keeps the historical located rejection of a non-constant-size heap
// allocation. Narrow by design: only write-fill-then-consume buffers lift.
#include <stdlib.h>
int puts(const char *);

void f(unsigned int len) {
  char *a = malloc(len + 1);
  for (int i = 0; i < len; ++i)
    a[i] = 'a';
  a[len] = '\0';
  char x = a[0]; // a byte read: disqualifies the lift
  (void)x;
  puts(a);
  free(a);
}
// CHECK: error: unsupported: allocation size is not a compile-time constant
