// Subsystem A of the FR-59 workspace partition test: leaf crate (no
// dependencies), with a file-static exercising the per-TU tag (private in
// the crate, never exported) and two externally visible functions the
// other crates call.

#include "pair.h"

static int scale2(int x) { return x * 2; }

int pair_sum(struct Pair p) { return scale2(p.x) + p.y; }

int make_val(int x) { return x + 1; }
