// REQUIRES: cargo
// CTS-S (00189) devirtualization, differential end-to-end test: transpile
// to a cargo crate, build it, and compare its stdout against the natively
// compiled C program. A never-reassigned global function pointer to a
// local function and one to the hosted variadic fprintf are both
// devirtualized; all formatted output flows through `fprintfptr(stdout,
// ...)` inside a data-dependent loop (fprintf itself is never called
// directly, mirroring 00189), mixed with direct printf calls in the same
// function. main returns 0 and reports everything via the printed lines,
// so lit's per-command exit-code checking covers both runs and diff
// covers the observable behavior. The program has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/fnptr_devirt > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdio.h>

int scale(int x) { return x * 3 + 1; }

int (*const scaleptr)(int) = &scale;

/* To test what this is supposed to test, the destination function
   (fprintf here) must not be called directly anywhere in the test. */
int (*fprintfptr)(FILE *, const char *, ...) = &fprintf;

int main(void) {
  int i;
  int acc = 0;
  for (i = 0; i < 6; i++) {
    acc = acc + scaleptr(i);
    fprintfptr(stdout, "i=%d scaled=%d acc=%d\n", i, scaleptr(i), acc);
  }
  fprintfptr(stdout, "total=%d\n", acc);
  printf("direct total=%d\n", acc);
  fprintfptr(stdout, "deref %d\n", (*scaleptr)(acc));
  return 0;
}
