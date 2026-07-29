// Companion translation unit of ../item-graph-multi-tu.c (TU index 1). It
// declares its OWN file-`static` `tally` and `bump`, spelled exactly like
// the ones in TU 0, plus an external `entry` that calls into TU 0's
// external `shared_step`.
#include "item-graph-multi-tu.h"

static int tally;

static void bump(void) { tally = tally + 2; }

int entry(void) {
  bump();
  return shared_step();
}
