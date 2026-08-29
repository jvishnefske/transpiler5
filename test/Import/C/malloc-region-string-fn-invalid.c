// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/struct-alloc.c 2>&1 | FileCheck %s --check-prefix=STRUCTALLOC
// RUN: not emitrust-import-c %t/int-alloc.c 2>&1 | FileCheck %s --check-prefix=INTALLOC
// RUN: not emitrust-import-c %t/pool-handle.c 2>&1 | FileCheck %s --check-prefix=POOL
// RUN: not emitrust-import-c %t/memcpy-same-alloc.c 2>&1 | FileCheck %s --check-prefix=MEMCPYSAME
// RUN: not emitrust-import-c %t/strcpy-same-alloc.c 2>&1 | FileCheck %s --check-prefix=STRCPYSAME
// RUN: not emitrust-import-c %t/strchr-alloc.c 2>&1 | FileCheck %s --check-prefix=STRCHRALLOC
// RUN: not emitrust-import-c %t/slice-arg-shared.c 2>&1 | FileCheck %s --check-prefix=SLICEARGSHARED
// RUN: not emitrust-import-c %t/slice-arg-mut.c 2>&1 | FileCheck %s --check-prefix=SLICEARGMUT
// RUN: not emitrust-import-c %t/slice-arg-typed.c 2>&1 | FileCheck %s --check-prefix=SLICEARGTYPED

// FR-146 frontier. Admitting ALLOCATION-BACKED regions as <string.h>
// arguments admits exactly the byte-typed, distinct-allocation shapes the
// existing helpers render as safe Rust; everything on the other side of
// that line keeps a LOCATED rejection. Every one of these inputs
// SEGFAULTED the importer before the FR (a null `VarDecl` dereferenced
// while formatting the region rejection in `emitCharRegionSlice`), which
// is why they are pinned here: a crash has no location, no wording, and
// no recovery, and it is the regression this file exists to prevent.
//
// Note that all six inputs are plain strict-mode C with no flags — the
// crash was never `--recover`-specific.

// A malloc'd STRUCT pointer (the uthash `memset(node, 0, sizeof *node)`
// shape, inside its for loop): the allocation's backing is a
// `[1 x !emitrust.struct<...>]`, not a byte array, so there is no byte
// region to borrow. Rejects with the historical char-array wording.
//--- struct-alloc.c
#include <stdlib.h>
#include <string.h>
struct N { int k; int v; };
int f(void) {
  int s = 0;
  for (int i = 0; i < 2; ++i) {
    struct N *node = (struct N *)malloc(sizeof(struct N));
    memset(node, 0, sizeof *node);
    node->k = i;
    s += node->k;
  }
  return s;
}
// STRUCTALLOC: struct-alloc.c:8:5: error: unsupported: string function argument must designate a char array

// A malloc'd `int *` is a typed-integer allocation. The FR-97 word-splat
// memset image covers LOCAL typed ARRAYS and member arrays only (it
// explicitly excludes backing-region pointers), so a typed allocation
// keeps the char-array rejection rather than silently taking a byte
// image over a differently-sized element.
//--- int-alloc.c
#include <stdlib.h>
#include <string.h>
int f(void) {
  int *p = (int *)malloc(8);
  memset(p, 0, 8);
  return p[0];
}
// INTALLOC: int-alloc.c:5:3: error: unsupported: string function argument must designate a char array

// A promoted node-pool HANDLE (W4.2e Part B / FR-39): its backing is the
// shared `emitrust.collection` place, not an array place, so it declines
// to the same located rejection instead of being borrowed as bytes.
//--- pool-handle.c
#include <stdlib.h>
#include <string.h>
struct Node { int val; struct Node *next; };
int sum_list(void) {
  struct Node *head = NULL;
  for (int i = 0; i < 5; i++) {
    struct Node *n = malloc(sizeof(struct Node));
    memset(n, 0, sizeof(struct Node));
    n->val = i;
    n->next = head;
    head = n;
  }
  int sum = 0;
  for (struct Node *c = head; c; c = c->next)
    sum += c->val;
  return sum;
}
// POOL: pool-handle.c:8:5: error: unsupported: string function argument must designate a char array

// Source and destination in the SAME allocation. Two allocation regions
// have no named base to collide on — the historical same-object check
// compares `VarDecl`s, and both are null here — so the collision is
// caught on the backing place instead. The array `copy_within`
// refinement is unavailable (a pointer's cursor into an allocation is
// not a provable whole region), and the naive two-borrow form is rustc
// E0502, so the pair rejects located.
//--- memcpy-same-alloc.c
#include <stdlib.h>
#include <string.h>
int f(void) {
  char *p = (char *)malloc(8);
  memcpy(p, p + 1, 2);
  return p[0];
}
// MEMCPYSAME: memcpy-same-alloc.c:5:3: error: unsupported: memcpy source and destination point into the same allocation

// The str*-family caller has the same collision and the same wording.
//--- strcpy-same-alloc.c
#include <stdlib.h>
#include <string.h>
int f(void) {
  char *p = (char *)malloc(8);
  strcpy(p, p);
  return p[0];
}
// STRCPYSAME: strcpy-same-alloc.c:5:3: error: unsupported: strcpy source and destination point into the same allocation

// A `strchr` result printed with `%s` re-slices at the found index, and
// that re-slice keeps only the base and the literal backing — an
// allocation region has neither, so the printed region resolves to
// nothing. It rejects located (this is the shape that would need the
// allocation backing threaded through the strchr re-slice; out of scope
// here, where the invariant is only that no shape crashes).
//--- strchr-alloc.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int f(void) {
  char *p = (char *)malloc(8);
  strcpy(p, "abc");
  printf("%s\n", strchr(p, 'b'));
  return 0;
}
// STRCHRALLOC: strchr-alloc.c:7:18: error: unsupported: string function argument over a pointer with no importable region

// Passing an allocation-backed pointer to a USER-DEFINED function is the
// second null-base crash site (`emitBorrowArgument`): the same base-less
// region, the same dereference of a null `VarDecl`. The region itself is
// representable as a reslice of the backing, but the caller's aliasing
// guard is keyed on the argument's `VarDecl` root, which an allocation
// region has none of — `f(p, p)`, `f(p, p + 1)` and `f(p, q)` after
// `q = p` would all emit two borrows of one backing array and fail only
// as rustc E0499/E0502 in the emitted crate. So the shape rejects located
// until that aliasing key exists: a diagnostic here is strictly better
// than a crash, and strictly better than a crate that does not build.
//--- slice-arg-shared.c
#include <stdlib.h>
int first(const char *b) { return b[0]; }
int f(void) {
  char *p = (char *)malloc(8);
  p[0] = 7;
  return first(p);
}
// SLICEARGSHARED: slice-arg-shared.c:6:10: error: unsupported: passing a pointer into a heap allocation as a slice argument

// The mutable-parameter spelling reaches the same guard.
//--- slice-arg-mut.c
#include <stdlib.h>
void fill(char *b) { b[0] = 3; }
int f(void) {
  char *p = (char *)malloc(8);
  fill(p);
  return p[0];
}
// SLICEARGMUT: slice-arg-mut.c:5:3: error: unsupported: passing a pointer into a heap allocation as a slice argument

// So does a typed (non-byte) allocation.
//--- slice-arg-typed.c
#include <stdlib.h>
int head(int *b) { return b[0]; }
int f(void) {
  int *p = (int *)malloc(8);
  p[0] = 7;
  return head(p);
}
// SLICEARGTYPED: slice-arg-typed.c:6:10: error: unsupported: passing a pointer into a heap allocation as a slice argument
