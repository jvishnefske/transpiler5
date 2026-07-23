// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// W4.2e Part B (FR-39): a function that RETURNS a node pointer would let the
// pooled node escape its function-scope backing (a dangling cursor), so the
// pool is not promoted and the returned pointer keeps its located rejection
// (this is what keeps binary-tree out of scope).
#include <stdlib.h>
struct Node { int val; struct Node *next; };
struct Node *make(void) {
  struct Node *n = malloc(sizeof(struct Node));
  n->val = 1;
  n->next = NULL;
  return n;
}
// CHECK: error: unsupported: returned pointer value
