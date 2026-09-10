// REQUIRES: cargo
// FR-230, the ROUNDING'S BEHAVIOR PIN. This is the corpus
// B01_synthetic/018_stack_buffer_overflow_loop1 `bad()` path verbatim:
// `alloca(10)` for an `int *`, then ten int-sized writes -- forty bytes
// into a ten-byte object.
//
// THAT ACCESS IS UNDEFINED IN C, which is the whole reason this file exists
// and the reason the native binary is deliberately NOT run and NOT diffed.
// C owes no answer, so any behavior conforms; what FR-230 chooses is the
// LOUD one. `ceil(10 / 4)` gives a `[i32; 3]` backing and Rust's own bounds
// check fires on the first out-of-range element, exactly as FR-229's
// over-length byte-view read panics rather than reading stack garbage, and
// that precedent is why the rounding was accepted instead of a refusal.
//
// The invariant pinned is therefore NEGATIVE and precise: the rounded
// allocation must never quietly absorb the overrun. If a later change
// rounds to something generous (a power of two, a "safety" pad, the
// requested byte count reinterpreted as elements), the writes fit, no panic
// fires, the exit code goes to 0 and this test fails -- which is the point.
// A silent stack scribble is the one outcome that must not be reachable.
//
// The exit code is the hard pin. The panic wording names both the length
// and the index, so it pins the ROUNDED EXTENT (3) and the FIRST
// out-of-range element (3) at once. `before` must appear on stdout first:
// Rust's stdout is line-buffered, so a missing prefix would mean the panic
// beat the writes and the failure is somewhere other than where it claims.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: sh -c '%t.crate/target/release/alloca_overrun_bounds_panic > %t.out 2> %t.err; echo rc=$? > %t.rc'
// RUN: FileCheck %s --check-prefix=RC < %t.rc
// RUN: FileCheck %s --check-prefix=OUT < %t.out
// RUN: sh -c 'grep -c panicked %t.err' | FileCheck %s --check-prefix=ONCE
// RUN: FileCheck %s --check-prefix=ERR < %t.err
//
// RC:   rc=101
// OUT: before
// OUT-NOT: after
// ONCE: 1
// ERR: index out of bounds: the len is 3 but the index is 3

#include <alloca.h>

int printf(const char *, ...);

/* `bad()` from the corpus case, seeded so nothing folds. */
static void bad(int seed) {
  int *data;
  data = (int *)alloca(10);
  {
    int source[10];
    int i;
    for (i = 0; i < 10; i++)
      source[i] = i * seed;
    for (i = 0; i < 10; i++)
      data[i] = source[i];
    printf("%d\n", data[0]);
  }
}

int main(int argc, char **argv) {
  printf("before\n");
  bad(argc);
  printf("after\n");
  return 0;
}
