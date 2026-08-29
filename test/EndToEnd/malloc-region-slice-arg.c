// REQUIRES: cargo
// FR-147 differential end-to-end test. An ALLOCATION-BACKED pointer passed
// to a USER-DEFINED function with a slice parameter. Before the FR this
// whole file was the located rejection `unsupported: passing a pointer into
// a heap allocation as a slice argument` (and before FR-146, an importer
// SEGFAULT), so there is no historical output to compare against — only the
// native program can say whether the new lowering is right. `cargo build`
// success is compile-only here and proves nothing: every property below
// fails as a WRONG NUMBER, not as a compile error. The stdout diff against
// the clang-built native binary is the oracle.
//
// Four properties the diff is built to catch:
//   * WINDOW placement — the argument borrow starts at the POINTER's own
//     cursor, not at the start of the allocation. `bump(p + off, ...)` with
//     a runtime `off` writes the wrong bytes if the cursor is dropped.
//   * WRITE-THROUGH — a mutable slice parameter must borrow the caller's
//     backing, not a copy; a copy loses every callee write silently.
//   * DISTINCT allocations — `copy(r, p, n)` passes two heap regions in ONE
//     call. They are separate backings, so the call is admitted; swapping
//     which backing each argument names would still compile.
//   * INITIALIZER-carried cursor — `char *q = p + off;` unites `q` into
//     `p`'s allocation region, and that DECLARATION used to drop its
//     initializer outright and bind `q` at cursor 0 (found while writing
//     this test; the assignment spelling `q = p + off` was already right).
//     `sum_from(q, ...)` reads the wrong window if the cursor is lost.
//
// The buffer contents, the fill byte, the OFFSET and every length derive
// from argc, so no constant folding can pre-compute the answer and hide a
// miscompile; the second RUN pair re-seeds through extra argv words.
// Deterministic, no UB: both allocations are fully memset before any read,
// and every window stays inside 16 bytes.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/malloc_region_slice_arg > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b c > %t.native4.out
// RUN: %t.crate/target/release/malloc_region_slice_arg a b c > %t.rust4.out
// RUN: diff %t.native4.out %t.rust4.out

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A read-only slice parameter. */
static int sum_from(const char *b, int n) {
  int total = 0;
  int i;
  for (i = 0; i < n; ++i)
    total += b[i];
  return total;
}

/* A mutable slice parameter: every write has to land in the CALLER's
   backing array, at the caller's own cursor. */
static void bump(char *b, int n, int delta) {
  int i;
  for (i = 0; i < n; ++i)
    b[i] = (char)(b[i] + delta);
}

/* Two heap regions in one call. Distinct allocations, so two simultaneous
   borrows are legal — and each must name its OWN backing. */
static void copy(char *dst, const char *src, int n) {
  int i;
  for (i = 0; i < n; ++i)
    dst[i] = src[i];
}

/* A TYPED (non-byte) allocation on the same path. */
static int total_ints(int *b, int n) {
  int total = 0;
  int i;
  for (i = 0; i < n; ++i)
    total += b[i];
  return total;
}

static void show(const char *tag, char *buf) {
  int i;
  printf("%s", tag);
  for (i = 0; i < 16; ++i)
    printf(" %d", buf[i]);
  printf("\n");
}

static void run(int seed) {
  char *p = (char *)malloc(16);
  char *r = (char *)malloc(16);
  int off = seed % 4;
  int n = seed % 5 + 2;
  int fill = 65 + seed % 7;
  int i;

  /* malloc's contents are indeterminate in C; zero both buffers first so
     every byte the diff prints below is defined. */
  memset(p, 0, 16);
  memset(r, 0, 16);
  for (i = 0; i < 16; ++i)
    p[i] = (char)(fill + i);

  /* Whole-region argument, cursor 0. */
  printf("sum0 %d\n", sum_from(p, n));

  /* Argument at a RUNTIME cursor: the window starts at `off`. */
  printf("sumoff %d\n", sum_from(p + off, n));

  /* Mutable parameter at a runtime cursor: the writes must be visible
     through `p` afterwards, at exactly [off, off + n). */
  bump(p + off, n, seed % 3 + 1);
  show("bumped", p);

  /* Two DISTINCT allocations in one call. */
  copy(r, p + off, n);
  show("copied", r);

  /* A second pointer local united into p's region, bound by an
     INITIALIZER, then passed as an argument. */
  char *q = p + off;
  printf("sumq %d\n", sum_from(q, n));
  bump(q, 1, 5);
  show("bumpq", p);

  /* The assignment spelling of the same binding, for comparison. */
  char *z;
  z = p + off;
  printf("sumz %d\n", sum_from(z, n));

  /* A typed allocation. */
  int *ints = (int *)malloc(4 * sizeof(int));
  for (i = 0; i < 4; ++i)
    ints[i] = seed * (i + 1);
  printf("ints %d\n", total_ints(ints, 4));
  printf("intsoff %d\n", total_ints(ints + (seed % 2), 2));
}

int main(int argc, char **argv) {
  run(argc);
  run(argc + 3);
  return 0;
}
