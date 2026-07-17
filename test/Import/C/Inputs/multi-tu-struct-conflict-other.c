// Companion TU for multi-tu-struct-conflict.c: defines the same file-scope
// tag `T` with a different field shape, which the cross-TU dedup must
// reject (C requires compatible types across TUs; this is a genuine
// conflict, not block-scope shadowing).
struct T {
  long y;
};

int probe(void) {
  struct T t;
  t.y = 1;
  return (int)t.y;
}
