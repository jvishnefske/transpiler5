// Companion for multi-tu-gate-g1-fnptr-result-diverge.c: a SECOND function
// returning the address of a DIFFERENT global (objB, not objA), reassigned
// onto the SAME externally visible `p` this file's companion declares. A
// real whole-program merge of address-taken candidates must detect this
// divergence and reject it, not silently pick one base.
struct S {
  int x;
};
extern struct S *(*p)(void);
struct S objB;

struct S *get2(void) { return &objB; }
void rebind2(void) { p = &get2; }

int main(void) { return p()->x; }
