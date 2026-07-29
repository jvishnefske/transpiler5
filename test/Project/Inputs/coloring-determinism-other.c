// Second translation unit of the FR-41 determinism fixture; see
// test/Project/coloring-determinism.c for what it is for. Not run on its own
// (lit excludes `Inputs` directories from the test sweep).
#include "coloring-determinism.h"

void blocked_b(void) { __asm__(""); }

int wide(struct Held *h) { return h->tag; }

int plain_value(struct Plain *p) { return p->value; }

// Calls into the OTHER unit's Red function as well as this one's, so the
// cross-unit poison edge exists in both orderings.
int mixed(void) {
  blocked_a();
  blocked_b();
  return 0;
}
