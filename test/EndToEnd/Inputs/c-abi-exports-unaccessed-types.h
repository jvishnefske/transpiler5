/* FR-226: the ONE definition of the record whose ADDRESS crosses the C ABI in
   the c-abi-exports-unaccessed dlopen end-to-end test, included by all three
   legs -- the transpiled library source, the clang-built native oracle, and
   the dlopen host.

   Note what is and is not being shared here. Unlike the FR-182 structs test,
   NOTHING in this test depends on the two sides agreeing about this record's
   LAYOUT: the exported wrapper spells its parameter `*mut core::ffi::c_void`
   and never looks behind it. The header exists so the host can ALLOCATE an
   object of the C compiler's shape, hand its address across, and then check
   byte for byte that the callee left every one of its bytes alone -- which is
   the invariant this class actually promises. */

#ifndef EMITRUST_C_ABI_EXPORTS_UNACCESSED_TYPES_H
#define EMITRUST_C_ABI_EXPORTS_UNACCESSED_TYPES_H

/* The TRACTOR corpus record, reduced to the two members
   `SPX_initialize_hash_function`'s callers actually carry. Its alignment is 4,
   which matters below: the wild pointer the driver invents is a multiple of 4
   so that forming it stays free of alignment UB in the C input itself. */
typedef struct {
  unsigned char seed[32];
  int n;
} spx_ctx;

#endif
