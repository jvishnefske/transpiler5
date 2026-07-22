// RealWorld corpus (Track 4): singly-linked list built with malloc'd nodes
// bound to a LOCAL pointer, traversed, then freed. Demand signal for C99-46
// (dynamic memory: local malloc + free).
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
