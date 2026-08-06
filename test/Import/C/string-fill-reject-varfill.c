// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// FR-64 rejection (varying fill): a fill whose byte is NOT a compile-time
// constant (`a[i] = (char)i` varies with the induction) has no single
// `String::repeat` character, so the buffer is NOT lifted and keeps the
// historical located rejection of a non-constant-size heap allocation.
#include <stdlib.h>
int puts(const char *);

void f(unsigned int len) {
  char *a = malloc(len + 1);
  for (int i = 0; i < len; ++i)
    a[i] = (char)i; // varying fill: not a constant repeat character
  a[len] = '\0';
  puts(a);
  free(a);
}
// CHECK: error: unsupported: allocation size is not a compile-time constant
