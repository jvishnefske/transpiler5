// REQUIRES: cargo
// FR-146 differential end-to-end test. Hosted <string.h> calls over
// ALLOCATION-BACKED `char *` regions (`memset`/`memcpy`/`strcpy`/`strcat`/
// `strncpy`/`strlen`/`memcmp` on a malloc'd buffer) import as
// `emitrust.slice_of` of the allocation's synthesized backing array at the
// pointer's own cursor. Every one of these calls SEGFAULTED the importer
// before the FR, so there is no historical output to compare against —
// only the native program can say whether the new lowering is right, and
// `cargo build` success proves nothing here. The stdout diff against the
// clang-built native binary is the oracle.
//
// Two properties the diff is built to catch:
//   * WINDOW placement — the region borrow starts at the pointer's CURSOR,
//     so an off-by-one start writes the wrong bytes; every buffer byte is
//     printed as a signed decimal after every call, including the NUL
//     padding `strncpy` leaves behind.
//   * ALIASING — `char *q = p;` unites q with p's allocation region, and
//     every pointer local in one region must decompose against ONE backing
//     array. A per-variable backing (what the importer emitted before)
//     gave q a private zeroed array, so writes through q were invisible
//     through p: a silent miscompile that only a byte diff can see.
//
// The buffer LENGTHS, the fill BYTE and the copied CONTENTS all derive from
// argc, so no constant folding can pre-compute the answer and hide a
// miscompile; the second RUN pair re-seeds through extra argv words.
// Deterministic, no UB: both allocations are fully memset before any read.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/malloc_region_string_fn > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/malloc_region_string_fn a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void run(int seed) {
  char *p = (char *)malloc(16);
  char *r = (char *)malloc(16);
  int n = seed % 5 + 1;
  int fill = 65 + seed % 7;
  int i;

  /* malloc's contents are indeterminate in C; zero both buffers first so
     every byte the diff prints below is defined. */
  memset(p, 0, 16);
  memset(r, 0, 16);

  /* A runtime-length, runtime-byte memset prefix. */
  memset(p, fill, n);
  printf("memset");
  for (i = 0; i < 16; ++i)
    printf(" %d", p[i]);
  printf("\n");

  /* The str*-family caller, including a NONZERO destination cursor. */
  strcpy(p, "abc");
  strcat(p, "XY");
  strncpy(p + 6, "wxyz", n);
  printf("strcpy");
  for (i = 0; i < 16; ++i)
    printf(" %d", p[i]);
  printf(" len %d\n", (int)strlen(p));

  /* Copies BETWEEN two distinct allocations, at runtime counts and a
     runtime destination offset. */
  for (i = 0; i < 8; ++i)
    r[i] = (char)(97 + (i * (seed + 1)) % 7);
  memcpy(p, r, n);
  memcpy(p + n, "ZZ", 2);
  printf("copy");
  for (i = 0; i < 16; ++i)
    printf(" %d", p[i]);
  for (i = 0; i < 16; ++i)
    printf(" %d", r[i]);
  printf(" eq %d len %d\n", memcmp(p, r, 3) == 0, (int)strlen(r));

  /* A SECOND pointer local into the SAME allocation: every write through
     it must be visible through the first. */
  {
    char *q = p;
    strcpy(q, "hey");
    printf("alias %d %d %d %d\n", p[0], p[1], p[2], p[3]);
    memset(q, fill, n);
    printf("alias2");
    for (i = 0; i < 8; ++i)
      printf(" %d", p[i]);
    printf("\n");
  }

  free(p);
  free(r);
}

int main(int argc, char **argv) {
  run(argc);
  run(argc * 3);
  run(0);
  return 0;
}
