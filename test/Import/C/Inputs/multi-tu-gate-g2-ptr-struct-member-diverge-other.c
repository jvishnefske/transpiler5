// Companion for multi-tu-gate-g2-ptr-struct-member-diverge.c: binds the
// SAME externally visible `head.next` to a DIFFERENT object (`t2`, not
// `t1`) and reads it — the genuinely divergent cross-TU binding.
struct Node {
  int val;
  struct Node *next;
};
extern struct Node head;
struct Node t2;

void link2(void) { head.next = &t2; }
int use_head(void) { return head.next->val; }

int main(void) {
  link2();
  return use_head();
}
