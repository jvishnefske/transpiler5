// FR-58 slice 2 shape-equality companion A2 for ../link-merge-errors.c:
// repeats companion A's `struct S` BYTE-IDENTICALLY (same field name, same
// type) under a different using function, so linking A and A2 must dedup
// the definition silently. Excluded from test discovery by
// config.excludes = ["Inputs"].

struct S {
  int a;
};

int use_fa2(struct S s) { return s.a; }
