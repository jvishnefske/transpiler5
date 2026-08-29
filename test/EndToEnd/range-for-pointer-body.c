// REQUIRES: cargo
// FR-61f-c slice 1 differential end-to-end test: a counted `for` whose body
// READS a pointer now lifts to a Rust `for i in ..` head, and the pointer's
// decomposition cells are loaded ONCE in front of the loop instead of being
// re-read every iteration.
//
// This is a CODEGEN change, so the only oracle that can see it is this one.
// `cargo build` success proves nothing here: a stale hoisted cursor, a hoist
// that skipped a nullable discriminant, or a lift admitted for a pointer the
// body actually walks would all compile perfectly and print the wrong numbers.
// Every arm below therefore prints every value it computes, and every seed is
// derived from `argc` so no extent, cursor or trip count can fold at import
// time -- a constant-folded miscompile would agree with the native by
// accident.
//
// The arms are the admission clause from both sides:
//   ADMIT  read-only cursor parameter walked by index (the dominant shape)
//   ADMIT  nullable region, read-only -- BOTH polarities of the null, so a
//          dropped Option discriminant panics or prints the wrong sum
//   ADMIT  write THROUGH the pointer (`out[i] = ...`): the pointer is
//          invariant, the region it designates is not
//   ADMIT  string-literal region
//   ADMIT  heap (malloc) backing -- FR-146/147
//   REFUSE `p++` in the body: the cursor is not invariant, so the loop must
//          keep the `while` lowering and still count correctly
//   REFUSE mutation in the INNER loop of a nested pair (`stmtWritesVar`
//          recurses, so the OUTER loop must refuse too)
//   REFUSE `&p` taken and written through `*pp` in the body
// The refusal arms are byte-diffed exactly like the admit arms: a refusal that
// silently became an admission would be a miscompile, not a build failure.
//
// Deterministic, no UB on any path; every index stays in bounds.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n1.out
// RUN: %t.crate/target/release/range_for_pointer_body > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b > %t.n3.out
// RUN: %t.crate/target/release/range_for_pointer_body a b > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out
// RUN: %t.native a b c d e > %t.n6.out
// RUN: %t.crate/target/release/range_for_pointer_body a b c d e > %t.r6.out
// RUN: diff %t.n6.out %t.r6.out

#include <stdio.h>
#include <stdlib.h>

/* ADMIT: read-only cursor parameter walked by index. */
static int sum_param(const int *a, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += a[i];
  return s;
}

/* ADMIT: write through an invariant pointer. */
static void fill_through(int *out, int n, int seed) {
  for (int i = 0; i < n; i++)
    out[i] = i * seed + 1;
}

/* ADMIT: nullable region, read-only in the body. `pick` is argc-derived, so
   both polarities are live and the hoisted discriminant must survive. */
static int sum_nullable(int n, int pick) {
  static int arr[8] = {2, 3, 5, 7, 11, 13, 17, 19};
  int *p = 0;
  if (pick)
    p = arr;
  int s = 0;
  if (p == 0)
    return -1;
  for (int i = 0; i < n; i++)
    s += p[i];
  return s;
}

/* ADMIT: string-literal region. */
static int sum_literal(int n) {
  const char *t = "hoisted!";
  int s = 0;
  for (int i = 0; i < n; i++)
    s += t[i];
  return s;
}

/* REFUSE: `p++` in the body -- the cursor is written every iteration. */
static int walk_ptr(const int *a, int n) {
  const int *p = a;
  int s = 0;
  for (int i = 0; i < n; i++) {
    s += *p * (i + 1);
    p++;
  }
  return s;
}

/* REFUSE: the mutation is in the INNER loop, so the OUTER one must refuse too.
   `rows * cols` elements are consumed in one continuous walk. */
static int nested_inner_write(const int *a, int rows, int cols) {
  const int *r = a;
  int s = 0;
  for (int i = 0; i < rows; i++) {
    for (int j = 0; j < cols; j++) {
      s += *r * (i + 1) - j;
      r++;
    }
  }
  return s;
}

/* REFUSE: `&p` escapes into `pp`, and the body rebinds `p` through it. */
static int addr_taken(int *a, int n) {
  int *p = a;
  int **pp = &p;
  int s = 0;
  for (int i = 0; i < n; i++) {
    *pp = a + i;
    s += **pp * (i + 2);
  }
  return s;
}

/* ADMIT: a HEAP backing (FR-146/147). The malloc'd region's cursor cell is
   hoisted exactly like a parameter's; the write loop and the read-back loop
   both lift. */
static int heap_sum(unsigned int n, int seed) {
  int *a = malloc(n * sizeof(int));
  for (unsigned int i = 0; i < n; ++i)
    a[i] = (int)i * seed + 1;
  int s = 0;
  for (unsigned int i = 0; i < n; ++i)
    s += a[i] * (int)(i + 1);
  free(a);
  return s;
}

int main(int argc, char **argv) {
  int n = 4 + argc; /* 5, 7 or 10 -- always <= 16 */
  int seed = argc * 3 + 1;

  int buf[16];
  fill_through(buf, n, seed);
  for (int i = 0; i < n; i++)
    printf("buf[%d]=%d\n", i, buf[i]);

  printf("sum_param=%d\n", sum_param(buf, n));
  printf("heap=%d\n", heap_sum((unsigned int)n, seed));
  printf("heap0=%d\n", heap_sum(0u, seed));
  printf("nullable_some=%d\n", sum_nullable(argc + 1, 1));
  printf("nullable_none=%d\n", sum_nullable(argc + 1, 0));
  printf("literal=%d\n", sum_literal(argc + 2));
  printf("walk=%d\n", walk_ptr(buf, n));
  printf("nested=%d\n", nested_inner_write(buf, 2, n / 2));
  printf("addr=%d\n", addr_taken(buf, n));
  return 0;
}
