// REQUIRES: cargo
// FR-137: differential end-to-end test for the extern-redeclared POINTER
// global — the shape that segfaulted the importer (antirez/sds sds.h:37
// `extern const char *SDS_NOINIT;` + sds.c:42 its definition). The three
// test/Import/C/globals-pointer-extern-redecl*.c siblings pin the emitted
// MLIR textually; per CLAUDE.md only the stdout diff against the
// clang-built native can see a MISCOMPILE, and the interesting half of
// this fix is not "stops crashing" but "still RECORDS the whole-program
// fact". Both file-scope decl orders appear here on two different
// globals — `extern` BEFORE the definition (`cursor`) and `extern` AFTER
// it (`tail`) — because order-independence is the pinned property. Each
// binds a CTS-P4 single-base cursor at a NONZERO element (`&table[3]`,
// `&other[5]`): every value is read and written THROUGH the pointer
// globals, never through the arrays, so a fix that let the redeclaration
// clobber the recorded cursor start back to 0 shifts the printed window
// and the diff catches it. The literal-backed sds shape rides along with
// its own const backing array. All seeds derive from argc and from the
// first argv words so constant folding cannot pre-compute the windows;
// the second RUN pair re-seeds. Every argc-derived index is clamped into
// the live suffix of its array. Deterministic, no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/globals_pointer_extern_redecl > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/globals_pointer_extern_redecl a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>

/* Order A: the header idiom -- `extern` declaration BEFORE the definition. */
extern int table[8];
extern int *cursor;

int table[8];
int *cursor = &table[3];

/* Order B: definition BEFORE the trailing `extern` redeclaration. */
int other[8];
int *tail = &other[5];

extern int *tail;

/* The literal-backed sds shape (sds.h:37 + sds.c:42). */
extern const char *SDS_NOINIT;

const char *SDS_NOINIT = "SDS_NOINIT";

int main(int argc, char **argv) {
  int seed = argc * 7 + 1;
  int i, a, b, n, k;
  if (argc > 1) seed += (int)(unsigned char)argv[1][0];
  if (argc > 2) seed += 3 * (int)(unsigned char)argv[2][0];

  for (i = 0; i < 8; ++i) table[i] = seed * (i + 1) + i;
  for (i = 0; i < 8; ++i) other[i] = seed * 3 - i * 5;

  /* Writes through the cursors land at table[3] / other[5], never [0]. */
  cursor[0] = seed ^ 42;
  cursor[1] += seed;
  tail[0] = seed * 2;
  tail[2] -= seed;

  a = argc % 5; /* cursor[a] -> table[3 + a], 3..7: in bounds */
  b = argc % 3; /* tail[b]   -> other[5 + b], 5..7: in bounds */

  printf("cursor");
  for (i = 0; i <= a; ++i) printf(" %d", cursor[i]);
  printf("\ntail");
  for (i = 0; i <= b; ++i) printf(" %d", tail[i]);
  printf("\npick %d %d\n", cursor[a], tail[b]);

  n = 0;
  while (SDS_NOINIT[n] != 0) ++n;
  k = argc % n;
  printf("sds %d %c %d", n, SDS_NOINIT[k], (int)SDS_NOINIT[k] + seed);
  for (i = 0; i < n; ++i) printf(" %d", (int)SDS_NOINIT[i] ^ (seed & 15));
  printf("\n");
  return 0;
}
