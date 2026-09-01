/* FR-182: the shared header of the multi-TU C-ABI export regression. Both
   translation units include it, so both see `struct inner` -- and the second
   one to be imported takes the cross-TU dedup path, where no struct_def is
   created at all. */
#ifndef EMITRUST_C_ABI_STRUCTS_MULTI_TU_H
#define EMITRUST_C_ABI_STRUCTS_MULTI_TU_H
struct inner {
  int a;
  int b;
};
#endif
