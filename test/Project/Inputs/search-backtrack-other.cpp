// FR-43: TU 1 of `search-backtrack.cpp`. Nothing here is remarkable, which is
// the point: `shared_util` is fully inside the subset and calls nothing
// unusual, so it is the collateral the search must NOT give up while repairing
// a failure caused two translation units away.
#include "search-backtrack.h"

int shared_util(int value) { return value + 5; }
