// FR-58 shape-conflict companion A for ../link-merge-errors.c: defines
// `struct Box` with ONE field. Excluded from test discovery by
// config.excludes = ["Inputs"].

struct Box {
  int a;
};

int use_a(struct Box b) { return b.a; }
