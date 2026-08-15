// Companion TU for static-underscore-prefix.c: its own file-static
// `_helper` must survive the FR-73 underscore fold under the tu1_ tag,
// distinct from the main TU's tu0_helper.
static int _helper(int x) { return x + 10; }

int use_other(int x) { return _helper(x); }
