/* FR-208: the ONE definition of the record that crosses the C ABI in the
   c-abi-exports-mixed-case dlopen end-to-end test, included by all three
   legs -- the transpiled library source, the clang-built native oracle, and
   the dlopen host.

   The record's own spelling is deliberately already snake_case: this test's
   variable is the FUNCTION symbol, and a renamed record would only add noise
   to a diff whose whole point is which name `nm -D` carries. The HOST owns
   the storage and hands its address to the emitted cdylib, so the FR-182
   `#[repr(C)]` promise is exercised here exactly as it is in the sibling
   struct test -- a wrapper that exports the right NAME over the wrong LAYOUT
   would still be caught. */

#ifndef EMITRUST_C_ABI_EXPORTS_MIXED_CASE_TYPES_H
#define EMITRUST_C_ABI_EXPORTS_MIXED_CASE_TYPES_H

typedef struct spx_pair {
  int lo;
  int hi;
} spx_pair;

#endif
