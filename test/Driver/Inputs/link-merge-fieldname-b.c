// FR-58 slice 2 shape-equality companion B for ../link-merge-errors.c:
// `struct S` with the SAME layout as companion A (one int) but the field
// renamed to `b`. Field names are attributes of the definition, so this
// pair must CONFLICT at link even though the layouts agree. Excluded from
// test discovery by config.excludes = ["Inputs"].

struct S {
  int b;
};

int use_fb(struct S s) { return s.b; }
