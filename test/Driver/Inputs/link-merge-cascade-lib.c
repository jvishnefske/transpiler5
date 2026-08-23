struct S { volatile int v; };
int use_s(struct S *p) { return p ? 1 : 0; }
int cascade_fine(int a) { return a + 1; }
