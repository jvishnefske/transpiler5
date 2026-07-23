// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// W4.2e Part B (FR-39) soundness boundary: a malloc nested inside MORE than
// one loop would allocate the product of the trip counts, overflowing a
// single-loop-sized pool at runtime. Such a function is not promoted (the
// pool capacity must equal the total allocation count), so the node
// pointers fall through to the located region rejection -- never a
// too-small pool that would panic or miscompile.
#include <stdlib.h>
struct Node { int val; struct Node *next; };
int f(void) {
  struct Node *head = NULL;
  for (int i = 0; i < 5; i++)
    for (int j = 0; j < 3; j++) {
      struct Node *n = malloc(sizeof(struct Node));
      n->val = i * j;
      n->next = head;
      head = n;
    }
  int s = 0;
  for (struct Node *c = head; c; c = c->next)
    s += c->val;
  return s;
}
// CHECK: error: unsupported:
