// FR-58 slice 2 shape-equality companion A for ../link-merge-errors.c:
// defines `struct S` whose single int field is named `a`. Companion B keeps
// the IDENTICAL layout but renames the field, and companion A2 repeats this
// shape byte-identically. Excluded from test discovery by
// config.excludes = ["Inputs"].

struct S {
  int a;
};

int use_fa(struct S s) { return s.a; }
