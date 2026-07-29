// FR-43: the shared header of `search-determinism.c`. Everything the project
// declares is externally visible and none of it is file-`static`, on purpose:
// FR-40 tags an internal-linkage symbol with the index of its translation
// unit (`tu<i>_`), so a project containing one would legitimately RENAME items
// under a permutation of the input list and there would be nothing left to
// compare. With only external symbols the two orders describe literally the
// same items, and any difference in the trace is a real order dependency.
#ifndef SEARCH_DETERMINISM_H
#define SEARCH_DETERMINISM_H

// Declared, never defined: the whole-program failure that makes the search
// actually branch, so what is being pinned is the reproducibility of a search
// that made CHOICES, not of one that probed once and stopped.
int missing_a(int value);
int missing_b(int value);

int reads_a(int value);
int reads_b(int value);
int plain(int value);

#endif
