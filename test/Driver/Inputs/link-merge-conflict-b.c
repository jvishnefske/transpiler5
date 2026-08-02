// FR-58 shape-conflict companion B for ../link-merge-errors.c: defines
// `struct Box` with TWO fields, conflicting with companion A's shape.
// Excluded from test discovery by config.excludes = ["Inputs"].

struct Box {
  int a;
  int b;
};

int use_b(struct Box b) { return b.a + b.b; }
