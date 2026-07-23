// REQUIRES: cargo
// W4.2e Part B differential end-to-end test (FR-39): a singly-linked list
// built from malloc'd nodes bound to LOCAL pointers, traversed, then freed.
// The nodes promote to a fixed [Node; CAP] index-handle pool (capacity
// folded from the malloc loop's trip count); each node pointer is a
// nullable pool index handle and the self-ref `next` field renders as
// Option<usize>. `free` is a no-op (the pool drops at scope end). The crate
// output must byte-match the natively compiled C program, with zero unsafe.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/linked_list > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdio.h>
#include <stdlib.h>

struct Node {
  int val;
  struct Node *next;
};

int main(void) {
  struct Node *head = NULL;
  for (int i = 0; i < 5; i++) {
    struct Node *n = malloc(sizeof(struct Node));
    n->val = i;
    n->next = head;
    head = n;
  }
  int sum = 0;
  for (struct Node *c = head; c; c = c->next)
    sum += c->val;
  printf("%d\n", sum);
  struct Node *c = head;
  while (c) {
    struct Node *nx = c->next;
    free(c);
    c = nx;
  }
  return 0;
}
