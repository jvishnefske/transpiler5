// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// FR-65 rejection (char element stays out of the Vec domain): a `char` buffer is
// the String/byte domain — `planStringFill` (FR-64) claims the constant-fill
// string idiom first, and a char buffer that is byte-READ (not the string idiom)
// is deliberately NOT lifted to `Vec<char>`/`Vec<u8>` this wave. It keeps its
// historical located non-constant-size rejection. `planVecLift` only claims
// non-char scalar elements (int/short/long/float/double), so the arms are
// disjoint by element type.
#include <stdlib.h>

int f(unsigned int n) {
  char *a = malloc(n);
  for (unsigned int i = 0; i < n; ++i)
    a[i] = 1;
  int s = 0;
  for (unsigned int i = 0; i < n; ++i)
    s += a[i]; // byte read: not the string idiom, and char is not the Vec domain
  free(a);
  return s;
}
// CHECK: error: unsupported: allocation size is not a compile-time constant
