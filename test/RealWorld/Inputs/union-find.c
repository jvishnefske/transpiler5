// RealWorld corpus (Track 4): disjoint-set (union-find) ported from the Linux
// kernel (lib/union_find.c). A self-referential `struct uf_node *parent`,
// path compression in find, union-by-rank, and pointer-equality between two
// pointer locals. Demand signal for the returned-pointer blocker (uf_find
// returns a pointer to a caller-supplied node, not a whole-global) layered on
// the self-referential member-pointer chain. Deterministic, no UB.
#include <stdio.h>

struct uf_node {
  struct uf_node *parent;
  unsigned int rank;
};

void uf_node_init(struct uf_node *node) {
  node->parent = node;
  node->rank = 0;
}

struct uf_node *uf_find(struct uf_node *node) {
  struct uf_node *parent;
  while (node->parent != node) {
    parent = node->parent;
    node->parent = parent->parent;
    node = parent;
  }
  return node;
}

void uf_union(struct uf_node *node1, struct uf_node *node2) {
  struct uf_node *root1 = uf_find(node1);
  struct uf_node *root2 = uf_find(node2);

  if (root1 == root2)
    return;

  if (root1->rank < root2->rank) {
    root1->parent = root2;
  } else if (root1->rank > root2->rank) {
    root2->parent = root1;
  } else {
    root2->parent = root1;
    root1->rank++;
  }
}

int connected(struct uf_node *a, struct uf_node *b) {
  struct uf_node *root1 = uf_find(a);
  struct uf_node *root2 = uf_find(b);
  return root1 == root2 ? 1 : 0;
}

#define N 8

int main(void) {
  struct uf_node nodes[N];
  int i, j;

  for (i = 0; i < N; i++)
    uf_node_init(&nodes[i]);

  uf_union(&nodes[0], &nodes[1]);
  uf_union(&nodes[2], &nodes[3]);
  uf_union(&nodes[1], &nodes[2]);
  uf_union(&nodes[4], &nodes[5]);
  uf_union(&nodes[6], &nodes[7]);

  for (i = 0; i < N; i++)
    for (j = 0; j < N; j++)
      printf("connected(%d,%d)=%d\n", i, j, connected(&nodes[i], &nodes[j]));

  for (i = 0; i < N; i++)
    printf("rank[%d]=%u\n", i, nodes[i].rank);

  return 0;
}
