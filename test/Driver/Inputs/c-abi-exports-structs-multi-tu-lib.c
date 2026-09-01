/* FR-182: the FIRST translation unit of the multi-TU C-ABI export regression.
   It is the one that CLAIMS the emitted name `Inner`, so the second TU's own
   `struct inner` decl reaches the dedup path with the struct_def already
   built. */
#include "c-abi-exports-structs-multi-tu-shared.h"

int inner_sum(struct inner *i) { return i->a + i->b; }
