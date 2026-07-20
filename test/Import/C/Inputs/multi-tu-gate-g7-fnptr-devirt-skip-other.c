// Companion for multi-tu-gate-g7-fnptr-devirt-skip.c: a second, read-only
// caller of the SAME const-qualified `addptr` from a different TU. Since
// `addptr` is `const`, no TU (this one included) can ever reassign it, so
// once a whole-program view exists this is a genuinely safe shape to
// devirtualize — this companion introduces no conflicting write.
extern int (*const addptr)(int, int);

int other_call(void) { return addptr(10, 20); }
