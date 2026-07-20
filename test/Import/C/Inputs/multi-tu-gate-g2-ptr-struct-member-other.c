// Companion for multi-tu-gate-g2-ptr-struct-member.c: reads the pointer
// member of the externally visible `head` instance from a different TU
// than the one that binds it.
struct Node {
  int val;
  struct Node *next;
};
extern struct Node head;

int use_head(void) { return head.next->val; }

int main(void) { return use_head(); }
