// REQUIRES: cargo
// CTS-F (00210) local void* fn-holder, differential end-to-end test:
// transpile to a cargo crate, build it, and compare its stdout against
// the natively compiled C program. A local `void *` holds the address of
// a function and is only ever called through explicit casts to the exact
// signature — both 00210 attributed cast spellings — plus a second
// holder initialized with the decayed (no ampersand) spelling. The
// printed values flow through the returned data (a global seed recomputed
// from the first call's result feeds the second call), not constants
// alone, and main returns 0 only if all three indirect calls agree.
// The program has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/fnptr_void_local > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdio.h>

#define ATTR __attribute__((__noinline__))

int seed = 6;

int ATTR times7(void) { return seed * 7; }

int main(void) {
  void *fp = &times7;
  void *fp2 = times7; /* decayed spelling, second holder */
  int a;
  int b;

  a = ((ATTR int (*)(void))fp)();
  printf("%i\n", a);

  seed = a - 36; /* recompute the seed from the returned value */

  b = ((int (ATTR *)(void))fp)();
  printf("%i\n", b);

  return (a - b) + (b - ((int (*)(void))fp2)());
}
