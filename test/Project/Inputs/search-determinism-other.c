// FR-43: TU 1 of `search-determinism.c`. It holds one of the two symmetric
// halves of the ambiguity, so that swapping the two translation units swaps
// which half the declaration walk reaches first.
#include "search-determinism.h"

int reads_b(int value) { return missing_b(value) + 2; }

int plain(int value) { return value * 4; }
