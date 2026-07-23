// REQUIRES: cargo
// W4.2e Part A differential end-to-end test: a runtime-sized integer stack
// over a malloc'd buffer bound to a LOCAL pointer, pushed and popped
// through a synthesized fixed [i32; CAP] backing + i64 cursor, then freed
// (a no-op drop). The crate output must byte-match the natively compiled
// C program. `cap` is a foldable automatic local (never reassigned nor
// address-taken), so `malloc(cap * sizeof(int))` folds to a CAP of 8.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/malloc_stack > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdlib.h>

int printf(const char *, ...);

int main(void) {
  int cap = 8;
  int *stack = malloc(cap * sizeof(int));
  int top = 0;
  for (int i = 0; i < 5; i++)
    stack[top++] = i * i;
  int sum = 0;
  while (top > 0)
    sum += stack[--top];
  printf("%d\n", sum);
  free(stack);
  return 0;
}
