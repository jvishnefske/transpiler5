// RealWorld corpus (Track 4): a binary search tree with malloc'd nodes and a
// pointer-returning insert. Demand signal for C99-46 (dynamic memory) and
// C99-43 (a function returning a pointer to a heap object).
#include <stdio.h>
#include <stdlib.h>

struct Tree {
  int val;
  struct Tree *left, *right;
};

static struct Tree *insert(struct Tree *root, int v) {
  if (!root) {
    struct Tree *n = malloc(sizeof(struct Tree));
    n->val = v;
    n->left = NULL;
    n->right = NULL;
    return n;
  }
  if (v < root->val)
    root->left = insert(root->left, v);
  else
    root->right = insert(root->right, v);
  return root;
}

static void inorder(struct Tree *root) {
  if (!root)
    return;
  inorder(root->left);
  printf("%d ", root->val);
  inorder(root->right);
}

int main(void) {
  struct Tree *root = NULL;
  int vals[] = {5, 3, 8, 1, 4, 7, 9, 2};
  for (int i = 0; i < 8; i++)
    root = insert(root, vals[i]);
  inorder(root);
  printf("\n");
  return 0;
}
