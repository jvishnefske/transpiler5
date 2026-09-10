// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/vla-size.c 2>&1 | FileCheck %s --check-prefix=VLASIZE
// RUN: not emitrust-import-c %t/huge.c 2>&1 | FileCheck %s --check-prefix=HUGE
// RUN: not emitrust-import-c %t/zero.c 2>&1 | FileCheck %s --check-prefix=ZERO
// RUN: not emitrust-import-c %t/escape.c 2>&1 | FileCheck %s --check-prefix=ESCAPE
// RUN: not emitrust-import-c %t/aligned.c 2>&1 | FileCheck %s --check-prefix=ALIGNED
// RUN: emitrust-cc --emit=import --recover %t/ledger.c -o - 2>&1 | FileCheck %s --check-prefix=LEDGER

// FR-230's FRONTIER for `alloca`. Admitting `alloca` into the fixed-backing
// allocation model moves exactly one thing -- the set of callee names -- so
// every guard the model already had must still fire, LOCATED, on the stack
// spelling. The bar is "a located rejection is an acceptable floor; silently
// wrong or silently unbuildable is not", and a runtime-sized `alloca` is
// precisely the shape with no fixed backing to synthesize: if it slipped
// through, the importer would have to invent an extent, which is the
// silently-wrong direction.
//
// The `> 65536` element cap and the zero-size refusal are UNCHANGED by
// FR-230's round-up, and both are pinned here so a future widening of the
// rounding has to walk past them. The cap applies to the ROUNDED count.
//
// The LEDGER unit is the diagnostic-quality half of FR-230, and it is a
// BEFORE/AFTER pin, not decoration. `citedLineAllocates` did not list
// `alloca`, so a refused stack allocation raising the shared "non-address
// value" wording tagged `pointer-local-nonaddress` and sat in the census
// bucket next to four unrelated pointer-member loads -- five different
// features read as one lever, which is how FR-230 came to be opened on a
// lever that did not exist. Measured on the same input: the unpatched tool
// tags this `pointer-local-nonaddress`, the patched one `dynamic-memory`.
// A refusal's wording is part of the ranking instrument.

// A runtime-sized `alloca` -- the idiomatic use, and the one the fixed
// backing cannot represent.
//--- vla-size.c
#include <alloca.h>
int f(int n) {
  int *p = (int *)alloca(n * sizeof(int));
  p[0] = 1;
  return p[0];
}
// VLASIZE: vla-size.c:3:12: error: unsupported: allocation size is not a compile-time constant

// Past the element cap. 65537 elements is one over.
//--- huge.c
#include <alloca.h>
int f(void) {
  int *p = (int *)alloca(65537 * sizeof(int));
  p[0] = 1;
  return p[0];
}
// HUGE: huge.c:3:12: error: unsupported: allocation size does not fit the pointer's element type

// A zero-byte allocation has no elements to round up to, and an empty
// backing has no valid subscript. The round-up must not turn 0 into 1.
//--- zero.c
#include <alloca.h>
int f(void) {
  int *p = (int *)alloca(0);
  p[0] = 1;
  return p[0];
}
// ZERO: zero.c:3:12: error: unsupported: allocation size does not fit the pointer's element type

// The backing is FUNCTION-SCOPE, which is `alloca`'s lifetime EXACTLY -- so
// returning it is a dangling pointer in the C, and a rejection here rather
// than a silently widened lifetime.
//--- escape.c
#include <alloca.h>
int *f(void) {
  int *p = (int *)alloca(4 * sizeof(int));
  p[0] = 1;
  return p;
}
// ESCAPE: escape.c:5:3: error: unsupported: returned pointer value (only a returned function address has a representation; a cursor into a callee-local region would dangle)

// An OVER-ALIGNED stack allocation is not the same request: the synthesized
// backing has Rust's alignment for its element type and nothing carries the
// extra alignment, so this stays refused. It is also the shape that keeps
// the ledger unit below honest -- it raises the shared, ambiguous wording on
// a line that plainly allocates.
//--- aligned.c
int f(void) {
  int *p;
  p = (int *)__builtin_alloca_with_align(16, 32);
  p[0] = 1;
  return p[0];
}
// ALIGNED: aligned.c:3:14: error: unsupported: pointer assigned a non-address value

// The ledger tag. A refused stack allocation belongs in `dynamic-memory`
// with the rest of the allocation family, never in the generic pointer
// bucket where the census cannot tell it apart from a pointer-member load.
//--- ledger.c
int stack_block(void) {
  int *p;
  p = (int *)__builtin_alloca_with_align(16, 32);
  p[0] = 1;
  return p[0];
}
int fine(int a) { return a * 2; }
// LEDGER: stubbed 'stack_block' [dynamic-memory]
// LEDGER: blocker tabulation
// LEDGER-DAG: dynamic-memory 1
// LEDGER-NOT: pointer-local-nonaddress
