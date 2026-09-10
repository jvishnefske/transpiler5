// REQUIRES: cargo
// FR-230 (D + G) differential end-to-end test, and THE oracle for this
// increment: `cargo build` succeeding on an `alloca` program proves only
// that the crate compiles, and every way this change can go wrong is a
// wrong-answer bug that compiles cleanly.
//
// Three things are under the diff at once, and each one is a plausible
// miscompile rather than a build failure:
//
//   1. The BACKING EXTENT. `alloca(10 * sizeof(int))` must synthesize ten
//      i32 slots. A backing one element short reads zero where the native
//      reads the value it just wrote -- or panics -- and a backing scaled
//      by the wrong unit shifts every element.
//   2. The CURSOR RESET. `data = alloca(...)` re-zeroes the backing and
//      resets the cursor. The second call below re-enters the same
//      function, so a cursor that survived from the previous call would
//      offset every subsequent access.
//   3. The NULL DISCRIMINANT (G). `data = NULL;` before the allocation is
//      the assignment that FR-230 admits; the flag it writes must be
//      overwritten by the allocation, so the `if (data)` guard here takes
//      the true arm. A flag stuck false prints the wrong branch and builds
//      perfectly.
//
// Seeds derive from `argc`, so nothing here constant-folds: a folded
// program would agree with the native for the wrong reason. Every element
// is written before it is read, the allocation is never over-run, and the
// `alloca` object is used only inside the frame that allocated it -- no UB,
// so both compilers owe the same answer and the diff is a real oracle.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/alloca_null_assign_region > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <alloca.h>
#include <stdlib.h>

int printf(const char *, ...);

/* The corpus B01_synthetic/018_stack_buffer_overflow_loop1 `good()` shape:
   declare, null, allocate, fill from a local array, read back. */
static int fill_and_sum(int seed) {
  int *data;
  data = NULL;
  data = (int *)alloca(10 * sizeof(int));
  {
    int source[10];
    int i;
    for (i = 0; i < 10; i++)
      source[i] = (i + 1) * seed;
    for (i = 0; i < 10; i++)
      data[i] = source[i];
  }
  if (data != NULL)
    printf("bound\n");
  else
    printf("null\n");
  {
    int total = 0;
    int i;
    for (i = 0; i < 10; i++)
      total += data[i];
    printf("first=%d last=%d total=%d\n", data[0], data[9], total);
    return total;
  }
}

/* A walked cursor over the same allocation: `p + n` and `++p` must resolve
   against the synthesized backing at the right offsets. */
static int walk(int seed) {
  int *base;
  base = NULL;
  base = (int *)alloca(6 * sizeof(int));
  {
    int i;
    for (i = 0; i < 6; i++)
      base[i] = seed * (i + 2);
  }
  {
    int *p = base + 2;
    int acc = *p;
    p++;
    acc += *p;
    acc += p[2];
    printf("walk=%d\n", acc);
    return acc;
  }
}

int main(int argc, char **argv) {
  printf("%d\n", fill_and_sum(argc));
  printf("%d\n", fill_and_sum(argc + 3));
  printf("%d\n", walk(argc));
  return 0;
}
