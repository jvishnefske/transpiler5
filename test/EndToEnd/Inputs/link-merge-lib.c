// Companion translation unit for ../link-merge-e2e.c. Excluded from test
// discovery by config.excludes = ["Inputs"]. Defines every symbol the main
// TU consumes across the link: the extern global, the plain function, and
// the shared-header-struct consumer -- plus a file-static `scale` whose
// spelling collides with the main TU's own static, forcing the FR-58 merge
// to alpha-rename the per-TU tags apart.

#include "link-merge.h"

int shared_counter = 7;

static int scale(int x) { return x * 3; }

int add(int a, int b) { return a + b; }

int apply(struct Point p, enum Mode m) {
  if (m == MODE_SCALED)
    return scale(p.x) + p.y + shared_counter;
  return p.x + p.y;
}
