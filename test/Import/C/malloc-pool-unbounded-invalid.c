// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// W4.2e Part B (FR-39): a node pool whose malloc loop has a non-foldable
// trip count (a runtime bound `k`) has no determinable capacity, so the
// pool is not promoted and the node pointers fall through to the region
// model, which rejects the linked-structure shape located.
#include <stdlib.h>
struct Node { int val; struct Node *next; };
int f(int k) {
  struct Node *head = NULL;
  for (int i = 0; i < k; i++) {
    struct Node *n = malloc(sizeof(struct Node));
    n->val = i;
    n->next = head;
    head = n;
  }
  int s = 0;
  for (struct Node *c = head; c; c = c->next)
    s += c->val;
  return s;
}
// CHECK: error: unsupported:
