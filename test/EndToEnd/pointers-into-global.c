// REQUIRES: cargo
// CTS-P6 local pointers into global aggregates, differential end-to-end
// test: transpile to a cargo crate, build it, and compare its stdout
// against the natively compiled C program. The risky seam is staged-copy
// coherence: every element access through the pointer stages the global's
// whole value and writes store the copy back, so this test adversarially
// interleaves writes through the pointer with direct reads of the global
// (and direct writes with reads through the pointer), including function
// calls that touch the global directly between pointer accesses, runtime
// rebinds, cursor walks in both directions, compound assignments and
// ++/-- through the pointer, two pointers into the same global (equality
// and difference), a degenerate pointer to a global scalar, and a pointer
// into a function-local static array persisting across calls. Same-
// statement accesses stay read-only mixes (each access stages afresh, so
// they are exact); all cursors stay in bounds; the program has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/pointers_into_global > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int gx = 100;
int garr[6];

void direct_scale(int i, int f) { garr[i] = garr[i] * f; }

int direct_sum(void) {
  int s = 0;
  int i;
  for (i = 0; i < 6; i++)
    s = s + garr[i];
  return s;
}

int next_static(void) {
  static int sarr[3];
  int *c = sarr;
  c += 2;
  *c = *c + 1; /* sarr[2] counts the calls */
  return sarr[2];
}

int main(void) {
  int i;

  for (i = 0; i < 6; i++)
    garr[i] = i;

  /* Write through the pointer, read the global directly (and back). */
  int *p = garr;
  *p = 7;
  printf("w-then-r %d %d\n", garr[0], garr[1]);
  garr[1] = 21;
  printf("direct-then-p %d %d\n", *p, p[1]);

  /* Same-statement read-only mix of pointer and direct access. */
  printf("mix %d\n", p[2] + garr[2] + *p);

  /* Walks and compound writes interleaved with direct reads. */
  p++;
  p[1] += garr[1]; /* garr[2] += 21 */
  printf("compound %d %d\n", garr[2], p[1]);
  (*p)--; /* garr[1]-- through the staged copy */
  printf("dec %d %d\n", garr[1], *p);

  /* A function that writes the global directly between pointer uses. */
  direct_scale(1, 3);
  printf("callee-w %d\n", *p);
  *p = *p + 1;
  printf("callee-r %d\n", direct_sum());

  /* Runtime rebind, backward walk, and a second pointer into the same
     global: equality and difference. */
  int *q = &garr[5];
  q--;
  printf("q %d %d\n", *q, (int)(q - p));
  q = &garr[direct_sum() % 6];
  printf("rebound %d eq=%d\n", *q, p == q);

  /* Degenerate pointer to a global scalar. */
  int *s = &gx;
  *s = *s + garr[0];
  printf("scalar %d %d\n", gx, *s);

  /* Pointer into a function-local static array across calls. */
  printf("static %d %d %d\n", next_static(), next_static(), next_static());

  /* Final state read both ways. */
  printf("final:");
  for (i = 0; i < 6; i++)
    printf(" %d/%d", garr[i], *(garr + i));
  printf("\n");

  return 0;
}
