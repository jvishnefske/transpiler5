// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// FR-64 rejection (non-canonical loop): the user's original snippet advanced
// the WRONG variable in the increment (`++len` instead of `++i`), so the loop
// is not the canonical `[0,len)` counted fill (it never advances `i`). The
// recognizer requires a bare `++i`/`i++` on the induction, so this buggy loop
// falls through to the historical located rejection — correctly NOT lifted.
#include <stdlib.h>
int puts(const char *);

void f(unsigned int len) {
  char *a = malloc(len + 1);
  for (int i = 0; i < len; ++len) // ++len, not ++i: not a counted fill
    a[i] = 'a';
  a[len] = '\0';
  puts(a);
  free(a);
}
// CHECK: error: unsupported: allocation size is not a compile-time constant
